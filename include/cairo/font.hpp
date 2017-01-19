#pragma once

#include <list>
#include <set>
#include <string>

#include <cairo/cairo-ft.h>
#include <cairo/cairo-xcb.h>
#include <cairo/cairo.h>

#include "common.hpp"
#include "errors.hpp"
#include "settings.hpp"
#include "utils/math.hpp"
#include "utils/scope.hpp"
#include "utils/string.hpp"

POLYBAR_NS

namespace cairo {
  namespace details {
    static FT_Library g_ftlib;

    struct unicode_char {
      explicit unicode_char() : codepoint(0), offset(0), length(0) {}

      unsigned long codepoint;
      int offset;
      int length;
    };

    using unicode_charlist = std::list<unicode_char>;

    bool utf8_to_ucs4(const unsigned char* src, unicode_charlist& result_list) {
      if (!src) {
        return false;
      }
      const unsigned char* first = src;
      while (*first) {
        int len = 0;
        unsigned long result = 0;
        if ((*first >> 7) == 0) {
          len = 1;
          result = *first;
        } else if ((*first >> 5) == 6) {
          len = 2;
          result = *first & 31;
        } else if ((*first >> 4) == 14) {
          len = 3;
          result = *first & 15;
        } else if ((*first >> 3) == 30) {
          len = 4;
          result = *first & 7;
        } else {
          return false;
        }
        const unsigned char* next;
        for (next = first + 1; *next && ((*next >> 6) == 2) && (next - first < len); next++) {
          result = result << 6;
          result |= *next & 63;
        }
        unicode_char uc_char;
        uc_char.codepoint = result;
        uc_char.offset = first - src;
        uc_char.length = next - first;
        result_list.push_back(uc_char);
        first = next;
      }
      return true;
    }

    size_t ucs4_to_utf8(char* utf8, uint32_t ucs) {
      if (ucs <= 0x7f) {
        *utf8 = ucs;
        return 1;
      } else if (ucs <= 0x07ff) {
        *(utf8++) = ((ucs >> 6) & 0xff) | 0xc0;
        *utf8 = (ucs & 0x3f) | 0x80;
        return 2;
      } else if (ucs <= 0xffff) {
        *(utf8++) = ((ucs >> 12) & 0x0f) | 0xe0;
        *(utf8++) = ((ucs >> 6) & 0x3f) | 0x80;
        *utf8 = (ucs & 0x3f) | 0x80;
        return 3;
      } else if (ucs <= 0x1fffff) {
        *(utf8++) = ((ucs >> 18) & 0x07) | 0xf0;
        *(utf8++) = ((ucs >> 12) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 6) & 0x3f) | 0x80;
        *utf8 = (ucs & 0x3f) | 0x80;
        return 4;
      } else if (ucs <= 0x03ffffff) {
        *(utf8++) = ((ucs >> 24) & 0x03) | 0xf8;
        *(utf8++) = ((ucs >> 18) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 12) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 6) & 0x3f) | 0x80;
        *utf8 = (ucs & 0x3f) | 0x80;
        return 5;
      } else if (ucs <= 0x7fffffff) {
        *(utf8++) = ((ucs >> 30) & 0x01) | 0xfc;
        *(utf8++) = ((ucs >> 24) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 18) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 12) & 0x3f) | 0x80;
        *(utf8++) = ((ucs >> 6) & 0x3f) | 0x80;
        *utf8 = (ucs & 0x3f) | 0x80;
        return 6;
      } else {
        return 0;
      }
    }

    class ft_facelock {
     public:
      explicit ft_facelock(cairo_scaled_font_t* font) : m_font(font) {
        m_face = cairo_ft_scaled_font_lock_face(m_font);
      }
      ~ft_facelock() {
        cairo_ft_scaled_font_unlock_face(m_font);
      }

      operator FT_Face() const {
        return m_face;
      }

     private:
      cairo_scaled_font_t* m_font;
      FT_Face m_face;
    };
  }

  class font {
   public:
    explicit font(cairo_t* cairo, FcPattern* pattern, int offset)
        : m_cairo(cairo), m_pattern(pattern), m_offset(offset) {
      cairo_matrix_t fm;
      cairo_matrix_t ctm;
      cairo_matrix_init_scale(&fm, size(), size());
      cairo_get_matrix(m_cairo, &ctm);

      auto fontface = cairo_ft_font_face_create_for_pattern(m_pattern);
      auto opts = cairo_font_options_create();
      m_scaled = cairo_scaled_font_create(fontface, &fm, &ctm, opts);
      cairo_font_options_destroy(opts);
      cairo_font_face_destroy(fontface);

      auto status = cairo_scaled_font_status(m_scaled);
      if (status != CAIRO_STATUS_SUCCESS) {
        throw application_error(sstream() << "cairo_scaled_font_create(): " << cairo_status_to_string(status));
      }

      auto lock = make_unique<details::ft_facelock>(cairo_scaled_font_reference(m_scaled));
      auto face = static_cast<FT_Face>(*lock);

      if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == FT_Err_Ok) {
        return;
      } else if (FT_Select_Charmap(face, FT_ENCODING_BIG5) == FT_Err_Ok) {
        return;
      } else if (FT_Select_Charmap(face, FT_ENCODING_SJIS) == FT_Err_Ok) {
        return;
      }
    }

    size_t match(details::unicode_charlist& charlist) {
      auto lock = make_unique<details::ft_facelock>(cairo_scaled_font_reference(m_scaled));
      auto face = static_cast<FT_Face>(*lock);
      size_t available_chars = 0;
      for (auto&& c : charlist) {
        if (FT_Get_Char_Index(face, c.codepoint)) {
          available_chars++;
        } else {
          break;
        }
      }

      return available_chars;
    }

    virtual ~font() {
      if (m_scaled != nullptr) {
        cairo_scaled_font_destroy(m_scaled);
      }
      if (m_pattern != nullptr) {
        FcPatternDestroy(m_pattern);
      }
    }

    string name() const {
      return property("family");
    }

    string file() const {
      return property("file");
    }

    int offset() const {
      return m_offset;
    }

    double size() const {
      bool scalable;
      double px;
      property(FC_SCALABLE, &scalable);
      if (scalable) {
        property(FC_SIZE, &px);
      } else {
        property(FC_PIXEL_SIZE, &px);
        px = static_cast<int>(px + 0.5);
      }
      return px;
    }

    size_t render(const string& text) {
      cairo_set_scaled_font(m_cairo, cairo_scaled_font_reference(m_scaled));
      cairo_scaled_font_extents(cairo_scaled_font_reference(m_scaled), &m_extents);
      cairo_rel_move_to(m_cairo, 0.0, m_extents.height / 2.0 - m_extents.descent + offset());

      double x, y;
      cairo_get_current_point(m_cairo, &x, &y);

      cairo_glyph_t* glyphs{nullptr};
      cairo_text_cluster_t* clusters{nullptr};
      cairo_text_cluster_flags_t cf{};
      int nglyphs = 0, nclusters = 0;
      string utf8 = string(text);
      auto status = cairo_scaled_font_text_to_glyphs(cairo_scaled_font_reference(m_scaled), x, y, utf8.c_str(),
          utf8.size(), &glyphs, &nglyphs, &clusters, &nclusters, &cf);

      if (status != CAIRO_STATUS_SUCCESS) {
        throw application_error(sstream() << "cairo_scaled_font_status()" << cairo_status_to_string(status));
      }

      size_t bytes = 0;
      for (int g = 0; g < nglyphs; g++) {
        if (glyphs[g].index) {
          bytes += clusters[g].num_bytes;
        } else {
          break;
        }
      }

      if (bytes) {
        utf8 = text.substr(0, bytes);
        cairo_scaled_font_text_to_glyphs(
            m_scaled, x, y, utf8.c_str(), utf8.size(), &glyphs, &nglyphs, &clusters, &nclusters, &cf);
        cairo_show_text_glyphs(m_cairo, utf8.c_str(), utf8.size(), glyphs, nglyphs, clusters, nclusters, cf);
        cairo_rel_move_to(m_cairo, glyphs[nglyphs].x - x, 0.0);
      }

      return bytes;
    }

   protected:
    string property(string&& property) const {
      FcChar8* file;
      if (FcPatternGetString(m_pattern, property.c_str(), 0, &file) == FcResultMatch) {
        return string(reinterpret_cast<char*>(file));
      } else {
        return "";
      }
    }

    void property(string&& property, bool* dst) const {
      FcBool b;
      FcPatternGetBool(m_pattern, property.c_str(), 0, &b);
      *dst = b;
    }

    void property(string&& property, double* dst) const {
      FcPatternGetDouble(m_pattern, property.c_str(), 0, dst);
    }

    void property(string&& property, int* dst) const {
      FcPatternGetInteger(m_pattern, property.c_str(), 0, dst);
    }

   private:
    cairo_t* m_cairo;
    cairo_scaled_font_t* m_scaled{nullptr};
    cairo_font_extents_t m_extents{};
    FcPattern* m_pattern{nullptr};
    int m_offset{0};
  };

  /**
   * Match and create font from given fontconfig pattern
   */
  decltype(auto) make_font(cairo_t* cairo, string&& fontname, int offset) {
    static bool fc_init{false};
    if (!fc_init && !(fc_init = FcInit())) {
      throw application_error("Could not load fontconfig");
    } else if (FT_Init_FreeType(&details::g_ftlib) != FT_Err_Ok) {
      throw application_error("Could not load FreeType");
    }

    static auto fc_cleanup = scope_util::make_exit_handler([] {
      FT_Done_FreeType(details::g_ftlib);
      FcFini();
    });

    auto pattern = FcNameParse((FcChar8*)fontname.c_str());
    FcDefaultSubstitute(pattern);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);

    if (match == nullptr) {
      throw application_error("Could not load font \"" + fontname + "\"");
    }

#ifdef DEBUG_FONTCONFIG
    FcPatternPrint(match);
#endif

    return make_unique<font>(cairo, match, offset);
  }
}

POLYBAR_NS_END
