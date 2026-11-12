#pragma once

#include "Model.h"

namespace spike {

// engine.solve_intrinsics / validate_intrinsics / fit_turntable equivalents.
QJsonObject solveIntrinsics(const std::vector<Capture>& captures, const Config& config);
QJsonObject validateIntrinsics(const QJsonObject& model,
                               const std::vector<Capture>& captures,
                               const Config& config);
QJsonObject fitTurntable(const QJsonObject& model, const QJsonArray& observations,
                         const QJsonObject& reference, const Config& config);

} // namespace spike
