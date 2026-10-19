#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "用法: ncnn_calibration_images 原图列表 输出列表 宽 高\n";
        return 2;
    }
    try {
        const int width = std::stoi(argv[3]), height = std::stoi(argv[4]);
        if (width <= 0 || height <= 0) return 2;
        std::ifstream source(argv[1]);
        const auto destination = std::filesystem::absolute(argv[2]);
        const auto imageDir = destination.parent_path() / "letterbox";
        std::filesystem::create_directories(imageDir);
        std::ofstream list(destination);
        if (!source || !list) return 3;
        std::string path;
        int count = 0;
        while (std::getline(source, path)) {
            if (!path.empty() && path.back() == '\r') path.pop_back();
            if (path.empty()) continue;
            const cv::Mat bgr = cv::imread(path, cv::IMREAD_COLOR);
            if (bgr.empty()) {
                std::cerr << "校准图不可读取，序号=" << count << '\n';
                return 4;
            }
            // 与生产 letterboxBgr 同用 float/lround/居中整数 pad，不能拉伸原图。
            const float scale = std::min(static_cast<float>(width) / bgr.cols,
                                         static_cast<float>(height) / bgr.rows);
            const int dw = static_cast<int>(std::lround(bgr.cols * scale));
            const int dh = static_cast<int>(std::lround(bgr.rows * scale));
            cv::Mat resized, padded(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
            cv::resize(bgr, resized, cv::Size(dw, dh));
            resized.copyTo(padded(cv::Rect((width - dw) / 2, (height - dh) / 2, dw, dh)));
            const auto imagePath = imageDir / (std::to_string(count++) + ".bmp");
            if (!cv::imwrite(imagePath.string(), padded)) return 5;
            list << imagePath.string() << '\n';
        }
        if (count == 0 || !list) return 6;
        std::cout << "CALIBRATION_IMAGES count=" << count << " geometry=letterbox pad=0\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "校准图准备失败\n";
        return 1;
    }
}
