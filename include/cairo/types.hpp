#pragma once

#include "common.hpp"

POLYBAR_NS

namespace cairo {
  struct abspos {
    double x;
    double y;
  };
  struct relpos {
    double x;
    double y;
  };

  struct rect {
    double x;
    double y;
    double w;
    double h;
  };

  struct line {
    double x1;
    double y1;
    double x2;
    double y2;
    double w;
  };

  struct linear_gradient {
    double x0;
    double y0;
    double x1;
    double y1;
    vector<uint32_t> steps;
  };

  struct textblock {
    string contents;
    uint8_t fontindex;
  };

  class context;
  class surface;
}

POLYBAR_NS_END
