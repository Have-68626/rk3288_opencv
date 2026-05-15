#pragma once
#include <string>
#include <vector>
#include <exception>
#include <cstdint>
namespace cv {
  struct Size { Size(int, int){} };
  struct Scalar { Scalar(double, double, double){} };
  class Mat {
  public:
      Mat() {}
      Mat(int, int, int) {}
      bool empty() const { return false; }
      Mat clone() const { return *this; }
      void copyTo(Mat&) const {}
      bool isContinuous() const { return true; }
      unsigned char* data = nullptr;
  };
  class UMat : public Mat {
  public:
      Mat getMat(int) const { return Mat(); }
  };
  class Exception : public std::exception {
  public:
      const char* what() const noexcept override { return "cv::Exception"; }
  };
  class RNG {
  public:
      RNG(uint64_t) {}
      enum { UNIFORM = 0 };
      void fill(Mat&, int, double, double) {}
  };
  const int ACCESS_READ = 0;
  const int CV_8UC3 = 16;
  namespace ocl {
    inline void setUseOpenCL(bool) {}
    inline bool useOpenCL() { return false; }
    inline bool haveOpenCL() { return false; }
    class Device {
    public:
        static Device getDefault() { return Device(); }
        std::string name() const { return "mock"; }
        std::string vendorName() const { return "mock"; }
        std::string version() const { return "mock"; }
        int type() const { return 0; }
        bool available() const { return false; }
        enum { TYPE_DEFAULT=0, TYPE_CPU=1, TYPE_GPU=2, TYPE_ACCELERATOR=3 };
    };
  }
}
using cv::CV_8UC3;
