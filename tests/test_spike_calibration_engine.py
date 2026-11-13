"""Independent numerical and byte-ingestion regressions for the Spike tool."""
import base64
import copy
import io
import unittest
from unittest.mock import patch

import cv2
import numpy as np

from spike_calibration_tool.engine import (DEFAULT_CONFIG, validate_config, validate_capture,
    solve_intrinsics, validate_intrinsics, fit_turntable, query_rays)
from spike_calibration_tool.fixtures import demo_captures, DEMO_K
from spike_calibration_tool.importers import import_capture, apply_orientation, orient_points, _window_bounds


def file_record(name, binary):
    return {"name": name, "content_base64": base64.b64encode(binary).decode("ascii")}


def npy_record(values):
    buffer = io.BytesIO()
    np.save(buffer, values)
    return file_record("raw_spike.npy", buffer.getvalue())


def chessboard():
    image = np.full((480,640), 160, dtype=np.uint8)
    for y in range(7):
        for x in range(10):
            image[100+y*35:100+(y+1)*35, 125+x*35:125+(x+1)*35] = 240 if (x+y) % 2 else 15
    return image


class SpikeCalibrationEngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = validate_config({})
        cls.captures = demo_captures(cls.config)
        cls.model = solve_intrinsics(cls.captures, cls.config)

    def test_defaults_keep_only_spike_assembled_branch(self):
        config = validate_config({"operator": "工程师"})
        self.assertEqual(config["optics"]["split_ratio"], "1:1")
        self.assertEqual(config["operator"], "工程师")
        self.assertIsNone(config["acceptance"]["intrinsics_p95_px"])
        with self.assertRaises(ValueError):
            validate_config({"optics": {"shared_lens": False}})
        with self.assertRaises(ValueError):
            validate_config({"orientation": {"flip_x": "false"}})
        with self.assertRaises(ValueError):
            validate_config({"target": {"square_length_m": float("nan")}})

    def test_multiview_recovers_focal_length(self):
        estimated = np.asarray(self.model["K"])
        self.assertLess(abs(estimated[0,0]-DEMO_K[0,0]), 2.)
        self.assertLess(abs(estimated[1,1]-DEMO_K[1,1]), 2.)
        self.assertLess(self.model["diagnostics"]["fit_rms_px"], .1)
        self.assertGreater(self.model["diagnostics"]["coverage"]["coverage_fraction"], .8)
        self.assertEqual(self.model["evidence"], ["synthetic"])

    def test_test_points_do_not_affect_intrinsics_fit(self):
        other = copy.deepcopy(self.captures)
        for capture in other:
            if capture["split"] == "test":
                points = np.asarray(capture["image_points"])
                points[:,0] += 2*np.sin(np.arange(len(points)))
                capture["image_points"] = points.tolist()
        from spike_calibration_tool.engine import fit_camera_intrinsics
        with patch("spike_calibration_tool.engine.fit_camera_intrinsics", wraps=fit_camera_intrinsics) as fit:
            refit = solve_intrinsics(other, self.config)
        train = [c for c in self.captures if c["split"] == "train"]
        self.assertEqual(fit.call_args.args[0], [c["object_points"] for c in train])
        self.assertEqual(fit.call_args.args[1], [c["image_points"] for c in train])
        # OpenCV parallel reductions can differ below a millionth of a pixel;
        # exact solver inputs above prove that holdout data never enters fitting.
        np.testing.assert_allclose(refit["K"], self.model["K"], rtol=0, atol=1e-5)
        np.testing.assert_allclose(refit["distortion"], self.model["distortion"], rtol=0, atol=1e-6)

    def test_independent_validation_freezes_model_and_no_implicit_pass(self):
        original = copy.deepcopy(self.model)
        result = validate_intrinsics(self.model, self.captures, self.config)
        self.assertEqual(self.model, original)
        self.assertEqual(result["status"], "measured")
        self.assertFalse(result["hardware_qualified"])
        self.assertLess(result["p95_px"], .15)
        config = validate_config({"acceptance": {"intrinsics_p95_px": .2}})
        self.assertEqual(validate_intrinsics(self.model,self.captures,config)["status"], "passed")
        config["acceptance"]["intrinsics_p95_px"] = .001
        self.assertEqual(validate_intrinsics(self.model,self.captures,config)["status"], "failed")

    def test_six_views_required(self):
        with self.assertRaisesRegex(ValueError, "六"):
            solve_intrinsics(self.captures[:5], self.config)

    def test_disabled_and_undetected_excluded(self):
        caps = copy.deepcopy(self.captures[:6])
        caps[-1]["enabled"] = False
        with self.assertRaisesRegex(ValueError, "六"):
            solve_intrinsics(caps, self.config)

    def test_duplicate_and_frontal_poses_rejected(self):
        caps = [copy.deepcopy(self.captures[0]) for _ in range(6)]
        for i, cap in enumerate(caps):
            cap["id"] = f"repeat-{i}"
        with self.assertRaisesRegex(ValueError, "重复"):
            solve_intrinsics(caps,self.config)
        K = np.array([[600.,0,320],[0,600,240],[0,0,1]])
        obj = np.asarray(caps[0]["object_points"])
        for i, cap in enumerate(caps):
            xy = cv2.projectPoints(obj,np.zeros(3),np.array([-.1+i*.007,-.06,.7+i*.01]),K,np.zeros(5))[0].reshape(-1,2)
            cap["image_points"] = xy.tolist()
        with self.assertRaisesRegex(ValueError, "退化"):
            solve_intrinsics(caps,self.config)

    def test_overlapping_sequences_rejected(self):
        caps = copy.deepcopy(self.captures)
        caps[-1]["sequence_id"] = caps[0]["sequence_id"]
        with self.assertRaisesRegex(ValueError, "同一采集序列"):
            solve_intrinsics(caps,self.config)
        test = copy.deepcopy(self.captures[-1])
        test["sequence_id"] = self.model["train_sequence_ids"][0]
        with self.assertRaisesRegex(ValueError, "重叠"):
            validate_intrinsics(self.model,[test],self.config)

    def test_frontal_poses_with_noise_or_distortion_do_not_produce_false_model(self):
        from scripts.generate_mock_calibration_data import make_pose, capture, OBJ, SEED
        rng = np.random.default_rng(SEED)
        # Match the independent fixture's noise, without requiring generated files.
        tilted = [make_pose(rng, i) for i in range(32)]
        noise = [rng.normal(0, .04, (len(OBJ), 2)) for _ in tilted]
        for distortion in (np.zeros(5), None):
            with self.subTest(distortion=distortion):
                caps = []
                for i in range(24):
                    pose = make_pose(rng, i, front_facing=True, distortion=distortion)
                    c = capture(f"flat-{i}", f"flat-seq-{i}", "train",
                                np.asarray(pose["image_points_truth"]) + noise[i], None)
                    c["id"] = str(i)
                    caps.append(c)
                with self.assertRaisesRegex(ValueError, "姿态约束退化"):
                    solve_intrinsics(caps, self.config)

    def test_missing_validation_rejected(self):
        with self.assertRaisesRegex(ValueError, "验证"):
            validate_intrinsics(self.model,self.captures[:12],self.config)

    def test_renamed_training_observation_is_not_independent(self):
        observation=copy.deepcopy(self.captures[0])
        observation.update(id="renamed",name="renamed.png",sequence_id="claimed-independent",split="test")
        with self.assertRaisesRegex(ValueError,"重叠"):
            validate_intrinsics(self.model,[observation],self.config)

    def test_orientation_points_match_pixel_operation(self):
        image = np.arange(12).reshape(3,4)
        xy = [[0,0],[3,2],[2,1]]
        for rotation in (0,90,180,270):
            for flip in (False,True):
                orientation = {"rotation_deg": rotation,"flip_x": flip,"flip_y": not flip}
                transformed = apply_orientation(image,orientation)
                pts,size = orient_points(xy,[4,3],orientation)
                self.assertEqual(size,[transformed.shape[1],transformed.shape[0]])
                for raw,p in zip(xy,pts.astype(int)):
                    self.assertEqual(image[raw[1],raw[0]],transformed[p[1],p[0]])

    def test_rotated_and_mirrored_multiview_solve(self):
        config = validate_config({"orientation": {"rotation_deg":90,"flip_x":True}})
        captures = demo_captures(config)
        model = solve_intrinsics(captures,config)
        self.assertEqual(model["image_size"],[480,640])
        self.assertAlmostEqual(model["K"][0][0],610,delta=3.)
        self.assertAlmostEqual(model["K"][1][1],620,delta=3.)
        self.assertLess(validate_intrinsics(model,captures,config)["p95_px"], .15)

    def test_circle_targets_and_image_encodings(self):
        for kind, cols, rows in (("circles", 7, 5), ("asymmetric_circles", 4, 11)):
            image = np.full((600, 700), 255, np.uint8)
            for row in range(rows):
                for col in range(cols):
                    x = 2*col + row % 2 if kind == "asymmetric_circles" else col
                    cv2.circle(image, (80+x*40, 70+row*40), 11, 0, -1)
            config = validate_config({"target": {"kind": kind, "cols": cols, "rows": rows}})
            for extension in (".png", ".jpg", ".bmp", ".tiff"):
                with self.subTest(kind=kind, extension=extension):
                    raw = cv2.imencode(extension, image)[1].tobytes()
                    c = import_capture(file_record("circles"+extension, raw), config,
                                       split="train", sequence_id="circles", evidence="synthetic")
                    self.assertTrue(c["found"])
                    self.assertEqual(len(c["image_points"]), cols*rows)

    def test_image_import_detects_real_board(self):
        binary = cv2.imencode(".png",chessboard())[1].tobytes()
        capture = import_capture(file_record("board.png",binary),self.config,split="train",sequence_id="real-capture-1",evidence="hardware")
        self.assertTrue(capture["found"])
        self.assertEqual(len(capture["image_points"]),54)
        self.assertEqual(capture["evidence"],"hardware")
        self.assertTrue(capture["preview_base64"])
        self.assertEqual(capture["image_size"],[640,480])
        self.assertNotIn("saturated_fraction",capture["quality"]["metrics"])
        self.assertIn("preview_white_endpoint_fraction",capture["quality"]["metrics"])

    def test_capture_metadata_types_are_checked(self):
        for source,quality in (([],{}),({"sha256":[]},{}),({},[])):
            capture=copy.deepcopy(self.captures[0])
            capture.update(source=source,quality=quality)
            with self.assertRaises(ValueError):
                validate_capture(capture)

    def test_binary_npy_import_preserves_input_and_ticks(self):
        image = chessboard()>120
        spikes = np.stack([image]*12).astype(np.uint8)
        before = spikes.copy()
        capture = import_capture(npy_record(spikes),self.config,split="test",sequence_id="spike-board",evidence="hardware",options={"window_start_tick":2,"window_ticks":8})
        self.assertTrue(capture["found"])
        self.assertEqual(capture["source"]["window_start_tick"],2)
        self.assertEqual(capture["source"]["window_ticks"],8)
        self.assertEqual(capture["source"]["unit"],"spikes_per_window")
        self.assertFalse(capture["source"]["raw_data_modified"])
        np.testing.assert_array_equal(before,spikes)

    def test_npy_nonbinary_and_object_arrays_rejected(self):
        for data in (np.full((3,4,5),2,dtype=np.uint8),np.array([{"x":1}],dtype=object)):
            with self.assertRaises(ValueError):
                import_capture(npy_record(data),self.config,split="train",sequence_id="bad",evidence="hardware")

    def test_forged_npy_size_is_rejected_before_array_allocation(self):
        for shape in ((2**60, 20, 20), (8, 20, 20)):
            with self.subTest(shape=shape):
                stream = io.BytesIO()
                np.lib.format.write_array_header_1_0(stream, {"shape": shape, "fortran_order": False, "descr": "|u1"})
                with patch("spike_calibration_tool.importers.np.load") as load:
                    with self.assertRaises(ValueError):
                        import_capture(file_record("forged.npy", stream.getvalue()), self.config,
                                       split="train", sequence_id="bad", evidence="synthetic")
                    load.assert_not_called()

    def test_npz_disguised_as_npy_rejected(self):
        stream=io.BytesIO()
        np.savez(stream,spikes=np.zeros((4,20,20),dtype=np.uint8))
        with self.assertRaisesRegex(ValueError,"NPZ"):
            import_capture(file_record("disguised.npy",stream.getvalue()),self.config,split="train",sequence_id="bad",evidence="hardware")
        truncated=npy_record(np.zeros((4,20,20),dtype=np.uint8))
        truncated["content_base64"]=base64.b64encode(base64.b64decode(truncated["content_base64"])[:100]).decode("ascii")
        with self.assertRaises(ValueError):
            import_capture(truncated,self.config,split="train",sequence_id="bad",evidence="hardware")

    def test_rate_requires_period_and_window_must_exist(self):
        record = npy_record(np.zeros((8,20,20),dtype=np.uint8))
        for options in ({"method":"rate"},{"window_start_tick":10},{"window_ticks":0}):
            with self.assertRaises(ValueError):
                import_capture(record,self.config,split="train",sequence_id="bad",evidence="hardware",options=options)

    def test_default_spike_window_adapts_to_sensor_size(self):
        self.assertEqual(_window_bounds({},512,1000*1000),(0,100))
        self.assertEqual(_window_bounds({},128,1024*1024),(0,95))
        self.assertEqual(_window_bounds({},512,64*64),(0,256))
        self.assertEqual(_window_bounds({"window_start_tick":490},512,1000*1000),(490,22))
        with self.assertRaisesRegex(ValueError,"一亿"):
            _window_bounds({"window_ticks":256},512,1000*1000)

    def test_unknown_dat_and_partial_frame_rejected(self):
        with self.assertRaisesRegex(ValueError,"不猜测"):
            import_capture(file_record("camera.dat",b"abcd"),self.config,split="train",sequence_id="bad",evidence="hardware")
        with self.assertRaisesRegex(ValueError,"文件长度"):
            import_capture(file_record("camera.dat",b"abc"),self.config,split="train",sequence_id="bad",evidence="hardware",options={"layout_confirmed":True,"layout":"packed_flat","bitorder":"little","width":8,"height":2})

    def test_explicit_dat_layouts_and_stride(self):
        rng = np.random.default_rng(72)
        data = rng.integers(0,2,(4,31,37),dtype=np.uint8)
        for layout in ("packed_flat","row_packed"):
            for order in ("little","big"):
                packed = np.packbits(data.reshape(4,-1),axis=1,bitorder=order) if layout=="packed_flat" else np.packbits(data,axis=2,bitorder=order).reshape(4,-1)
                padded = np.pad(packed,((0,0),(0,3)))
                binary = b"HEAD"+padded.tobytes()
                capture = import_capture(file_record("raw.dat",binary),self.config,split="train",sequence_id="packed",evidence="hardware",options={"layout_confirmed":True,"layout":layout,"bitorder":order,"width":37,"height":31,"header_bytes":4,"frame_stride_bytes":padded.shape[1],"window_start_tick":1,"window_ticks":2})
                preview = cv2.imdecode(np.frombuffer(base64.b64decode(capture["preview_base64"]),np.uint8),cv2.IMREAD_GRAYSCALE)
                expected = data[1:3].sum(axis=0).astype(float)
                expected = ((expected-expected.min())/(expected.max()-expected.min())*255).astype(np.uint8)
                np.testing.assert_array_equal(preview,expected)

    def test_query_right_positive_up_positive_and_bounds(self):
        model={"K":[[600,0,320],[0,600,240],[0,0,1]],"distortion":[0]*5,"image_size":[640,480]}
        result=query_rays(model,[[320,240],[380,180],[260,300]])["points"]
        np.testing.assert_allclose(result[0]["ray_camera"],[0,0,1])
        self.assertGreater(result[1]["azimuth_deg"],0)
        self.assertGreater(result[1]["elevation_deg"],0)
        self.assertLess(result[2]["azimuth_deg"],0)
        self.assertLess(result[2]["elevation_deg"],0)
        self.assertAlmostEqual(np.linalg.norm(result[1]["ray_camera"]),1.)
        for points in ([[-1,0]],[[640,100]],[[float("nan"),1]]):
            with self.assertRaises(ValueError):
                query_rays(model,points)

    def _turntable(self,reference,sign=1):
        model={"K":[[700,0,320],[0,700,240],[0,0,1]],"distortion":[-.05,.01,.001,-.001,0],"image_size":[640,480]}
        zero=1.35
        observations=[]
        for i,q in enumerate([-9,-5,-1,3,7,-7,1,5]):
            if reference["mode"]=="far_field":
                expected=reference["reference_angle_deg"]-sign*q
            else:
                x,y,z=reference["target_position_m"]
                a=np.radians(sign*q)
                expected=np.degrees(np.arctan2(np.cos(a)*x-np.sin(a)*z-reference["camera_offset_m"][0],np.sin(a)*x+np.cos(a)*z-reference["camera_offset_m"][2]))
            az=np.radians(expected-zero)
            pixel=cv2.projectPoints(np.array([[np.tan(az),0.,1.]]),np.zeros(3),np.zeros(3),np.asarray(model["K"],float),np.asarray(model["distortion"],float))[0].reshape(-1,2)[0]
            observations.append({"pixel":pixel.tolist(),"encoder_deg":q,"split":"train" if i<5 else "test","sequence_id":"train-rotation" if i<5 else "test-rotation"})
        return model,observations,zero

    def test_far_field_zero_both_signs(self):
        for sign in (1,-1):
            reference={"mode":"far_field","reference_angle_deg":3.,"sign":sign,"far_field_confirmed":True}
            model,obs,zero=self._turntable(reference,sign)
            fitted=fit_turntable(model,obs,reference,self.config)
            self.assertAlmostEqual(fitted["zero_offset_deg"],zero,places=6)
            self.assertLess(fitted["test_p95_deg"],1e-5)
            self.assertEqual(fitted["status"],"measured")

    def test_finite_geometry_accounts_for_lever_arm(self):
        reference={"mode":"finite_distance","target_position_m":[.30,.04,1.75],"camera_offset_m":[.12,.01,.06],"sign":1}
        model,obs,zero=self._turntable(reference)
        fitted=fit_turntable(model,obs,reference,self.config)
        self.assertAlmostEqual(fitted["zero_offset_deg"],zero,places=6)
        self.assertLess(fitted["test_p95_deg"],1e-5)
        query=query_rays(model,[obs[0]["pixel"]],fitted,encoder_deg=0)
        self.assertNotIn("encoder_target_deg",query["points"][0])
        self.assertIn("direction_only_note",query["points"][0])
        wrong=fit_turntable(model,obs,{"mode":"far_field","reference_angle_deg":0,"far_field_confirmed":True},self.config)
        self.assertGreater(wrong["test_p95_deg"],.02)

    def test_turntable_rejects_unconfirmed_geometry_and_sequence_leak(self):
        reference={"mode":"far_field","reference_angle_deg":0,"sign":1,"far_field_confirmed":True}
        model,obs,_=self._turntable(reference)
        with self.assertRaisesRegex(ValueError,"明确确认"):
            fit_turntable(model,obs,{**reference,"far_field_confirmed":False},self.config)
        obs[-1]["sequence_id"]=obs[0]["sequence_id"]
        with self.assertRaisesRegex(ValueError,"独立采集"):
            fit_turntable(model,obs,reference,self.config)


if __name__ == "__main__":
    unittest.main()
