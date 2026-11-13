# Spike 标定工具应用图标

本次图标使用内置 `image_gen` 生成。图案由镜头环、中心对准点及四角定位框组成；深蓝底、白色主轮廓和青色中心点与标定工作台的视觉语言一致。图标外部保留透明通道。

- `spike-calibration.png`：1024 × 1024 RGBA，用于应用内展示。
- `spike-calibration.ico`：16、24、32、48、64、128、256 px，供 Windows 窗口、任务栏及 EXE 使用。
- `app.qrc`：Qt 资源路径 `:/app/icon.png` 和 `:/app/icon.ico`。
- `app.rc.in`：构建时嵌入 Windows 可执行文件图标。

生成结果仅使用 ImageMagick 进行尺寸及 ICO 格式转换，未改变图案；原始生成图片保留在生成工具的输出目录中。

## 生成提示词

```text
Use case: logo-brand.
Asset type: finished Windows desktop application icon for a scientific optical camera calibration workbench.
Primary request: create ONE polished, simple and highly recognizable application icon combining a camera lens, precise central calibration target and four alignment brackets. Professional engineering instrument, calm precision.
Composition: a single centered icon on a square 1024 by 1024 canvas, a deep navy rounded-square app tile with modest corner radius occupying about 88% of the canvas. Outside the tile must be genuinely transparent. Inside the tile use one broad clean white/light blue optical ring and a small cyan center point, with simple bold alignment corners; balanced geometric forms, generous negative space, crisp silhouette that survives 16px rendering.
Style: premium flat-vector-like geometric icon with restrained subtle blue shading, no photographic lens detail, no shiny 3D or chrome. Navy #10233F, blue #2563EB, cool white #F4F8FF and a small cyan #36D4DB accent.
Constraints: no words, no letters, no numbers, no watermark, no mockup, no scene, no border outside the tile, no tiny ticks or grids, no multiple variants. Only one final usable icon with actual transparent exterior.
```
