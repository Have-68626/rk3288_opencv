#pragma once
#include <algorithm>

namespace cv {

template<typename _Tp>
struct Rect_ {
    _Tp x, y, width, height;

    Rect_() : x(0), y(0), width(0), height(0) {}
    Rect_(_Tp _x, _Tp _y, _Tp _w, _Tp _h) : x(_x), y(_y), width(_w), height(_h) {}

    _Tp area() const { return width * height; }
};

typedef Rect_<int> Rect;
typedef Rect_<float> Rect2f;

}  // namespace cv
