#pragma once
#include "core.hpp"
namespace cv {
namespace dnn {
  class Net {
  public:
      void setInput(const Mat&) {}
      Mat forward(const std::string& = "") { return Mat(); }
      void setPreferableBackend(int) {}
      void setPreferableTarget(int) {}
  };
  inline Net readNet(const std::string&, const std::string& = "", const std::string& = "") { return Net(); }
  inline Mat blobFromImage(const Mat&, double, Size, Scalar, bool, bool) { return Mat(); }
  inline void blobFromImage(const Mat&, UMat&, double, Size, Scalar, bool, bool) {}
}
}
