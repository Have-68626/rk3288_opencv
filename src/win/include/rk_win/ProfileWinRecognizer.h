#pragma once

#include "IRecognizer.h"
#include "FaceDatabase.h"
#include "pipeline/RealtimeFacePipeline.h"

namespace rk_win {

// Windows 的配置入口复用核心模型装配，模板持久化仍沿用 DPAPI 保护的加密数据库。
class ProfileWinRecognizer final : public IRecognizer {
public:
    explicit ProfileWinRecognizer(rk_core::RealtimePipelineHooks hooks = rk_core::makeDefaultRealtimePipelineHooks());
    bool initialize(const std::filesystem::path& profileDir, const std::filesystem::path& databasePath, std::string& err);
    std::shared_ptr<const rk_core::RealtimePipelineSnapshot> snapshot() const;
    std::vector<FaceMatch> identify(const cv::Mat& bgr) override;
    bool enrollFromFrame(const std::string& personId, const cv::Mat& bgr, int samplesToTake, int& samplesTakenOut) override;
    bool saveDb() const override;
    void clearDb() override;
    double threshold() const override;
    void setThreshold(double t) override;
    std::vector<std::string> personIds() const override;
private:
    bool embedFace(const cv::Mat& bgr, const rk_core::RealtimeFaceBox& box, std::vector<float>& out) const;
    rk_core::RealtimeFacePipeline pipeline_;
    FaceDatabase db_;
    std::filesystem::path dbPath_;
    double threshold_ = 0.45;
    bool ready_ = false;
};

} // namespace rk_win
