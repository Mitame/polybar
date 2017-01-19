#include <xcb/xcb_icccm.h>
#include <algorithm>

#include "components/bar.hpp"
#include "components/config.hpp"
#include "components/parser.hpp"
#include "components/renderer.hpp"
#include "components/screen.hpp"
#include "components/taskqueue.hpp"
#include "components/types.hpp"
#include "events/signal.hpp"
#include "events/signal_emitter.hpp"
#include "utils/bspwm.hpp"
#include "utils/color.hpp"
#include "utils/factory.hpp"
#include "utils/math.hpp"
#include "utils/string.hpp"
#include "x11/atoms.hpp"
#include "x11/connection.hpp"
#include "x11/extensions/all.hpp"
#include "x11/tray_manager.hpp"
#include "x11/wm.hpp"

#if ENABLE_I3
#include "utils/i3.hpp"
#endif

POLYBAR_NS

using namespace signals::ui;
using namespace wm_util;

/**
 * Create instance
 */
bar::make_type bar::make(bool only_initialize_values) {
  // clang-format off
  return factory_util::unique<bar>(
        connection::make(),
        signal_emitter::make(),
        config::make(),
        logger::make(),
        screen::make(),
        tray_manager::make(),
        parser::make(),
        taskqueue::make(),
        only_initialize_values);
  // clang-format on
}

/**
 * Construct bar instance
 *
 * TODO: Break out all tray handling
 */
bar::bar(connection& conn, signal_emitter& emitter, const config& config, const logger& logger,
    unique_ptr<screen>&& screen, unique_ptr<tray_manager>&& tray_manager, unique_ptr<parser>&& parser,
    unique_ptr<taskqueue>&& taskqueue, bool only_initialize_values)
    : m_connection(conn)
    , m_sig(emitter)
    , m_conf(config)
    , m_log(logger)
    , m_screen(forward<decltype(screen)>(screen))
    , m_tray(forward<decltype(tray_manager)>(tray_manager))
    , m_parser(forward<decltype(parser)>(parser))
    , m_taskqueue(forward<decltype(taskqueue)>(taskqueue)) {
  string bs{m_conf.section()};

  // Get available RandR outputs
  auto monitor_name = m_conf.get(bs, "monitor", ""s);
  auto monitor_name_fallback = m_conf.get(bs, "monitor-fallback", ""s);
  auto monitor_strictmode = m_conf.get(bs, "monitor-strict", false);
  auto monitors = randr_util::get_monitors(m_connection, m_connection.screen()->root, monitor_strictmode);

  if (monitors.empty()) {
    throw application_error("No monitors found");
  }

  if (monitor_name.empty() && !monitor_strictmode) {
    auto connected_monitors = randr_util::get_monitors(m_connection, m_connection.screen()->root, true);
    if (!connected_monitors.empty()) {
      monitor_name = connected_monitors[0]->name;
      m_log.warn("No monitor specified, using \"%s\"", monitor_name);
    }
  }

  if (monitor_name.empty()) {
    monitor_name = monitors[0]->name;
    m_log.warn("No monitor specified, using \"%s\"", monitor_name);
  }

  bool name_found{false};
  bool fallback_found{monitor_name_fallback.empty()};
  monitor_t fallback{};

  for (auto&& monitor : monitors) {
    if (!name_found && (name_found = monitor->match(monitor_name, monitor_strictmode))) {
      m_opts.monitor = move(monitor);
    } else if (!fallback_found && (fallback_found = monitor->match(monitor_name_fallback, monitor_strictmode))) {
      fallback = move(monitor);
    }

    if (name_found && fallback_found) {
      break;
    }
  }

  if (!m_opts.monitor) {
    if (fallback) {
      m_opts.monitor = move(fallback);
      m_log.warn("Monitor \"%s\" not found, reverting to fallback \"%s\"", monitor_name, monitor_name_fallback);
    } else {
      throw application_error("Monitor \"" + monitor_name + "\" not found or disconnected");
    }
  }

  m_log.info("Loaded monitor %s (%ix%i+%i+%i)", m_opts.monitor->name, m_opts.monitor->w, m_opts.monitor->h,
      m_opts.monitor->x, m_opts.monitor->y);

  try {
    m_opts.override_redirect = m_conf.get<bool>(bs, "dock");
    m_conf.warn_deprecated(bs, "dock", "override-redirect");
  } catch (const key_error& err) {
    m_opts.override_redirect = m_conf.get(bs, "override-redirect", m_opts.override_redirect);
  }

  m_opts.dimvalue = m_conf.get(bs, "dim-value", 1.0);
  m_opts.dimvalue = math_util::cap(m_opts.dimvalue, 0.0, 1.0);

  // Build WM_NAME
  m_opts.wmname = m_conf.get(bs, "wm-name", "polybar-" + bs.substr(4) + "_" + m_opts.monitor->name);
  m_opts.wmname = string_util::replace(m_opts.wmname, " ", "-");

  // Load configuration values
  m_opts.origin = m_conf.get(bs, "bottom", false) ? edge::BOTTOM : edge::TOP;
  m_opts.spacing = m_conf.get(bs, "spacing", m_opts.spacing);
  m_opts.separator = m_conf.get(bs, "separator", ""s);
  m_opts.locale = m_conf.get(bs, "locale", ""s);

  try {
    auto padding = m_conf.get<decltype(m_opts.padding.left)>(bs, "module-padding");
    m_opts.padding.left = padding;
    m_opts.padding.right = padding;
  } catch (const key_error& err) {
    m_opts.padding.left = m_conf.get(bs, "padding-left", m_opts.padding.left);
    m_opts.padding.right = m_conf.get(bs, "padding-right", m_opts.padding.right);
  }

  try {
    auto margin = m_conf.get<decltype(m_opts.padding.left)>(bs, "module-margin");
    m_opts.module_margin.left = margin;
    m_opts.module_margin.right = margin;
  } catch (const key_error& err) {
    m_opts.module_margin.left = m_conf.get(bs, "module-margin-left", m_opts.module_margin.left);
    m_opts.module_margin.right = m_conf.get(bs, "module-margin-right", m_opts.module_margin.right);
  }

  if (only_initialize_values) {
    return;
  }

  // Load values used to adjust the struts atom
  m_opts.strut.top = m_conf.get("global/wm", "margin-top", 0);
  m_opts.strut.bottom = m_conf.get("global/wm", "margin-bottom", 0);

  // Load commands used for fallback click handlers
  vector<action> actions;
  actions.emplace_back(action{mousebtn::LEFT, m_conf.get(bs, "click-left", ""s)});
  actions.emplace_back(action{mousebtn::MIDDLE, m_conf.get(bs, "click-middle", ""s)});
  actions.emplace_back(action{mousebtn::RIGHT, m_conf.get(bs, "click-right", ""s)});
  actions.emplace_back(action{mousebtn::SCROLL_UP, m_conf.get(bs, "scroll-up", ""s)});
  actions.emplace_back(action{mousebtn::SCROLL_DOWN, m_conf.get(bs, "scroll-down", ""s)});
  actions.emplace_back(action{mousebtn::DOUBLE_LEFT, m_conf.get(bs, "double-click-left", ""s)});
  actions.emplace_back(action{mousebtn::DOUBLE_MIDDLE, m_conf.get(bs, "double-click-middle", ""s)});
  actions.emplace_back(action{mousebtn::DOUBLE_RIGHT, m_conf.get(bs, "double-click-right", ""s)});

  for (auto&& act : actions) {
    if (!act.command.empty()) {
      m_opts.actions.emplace_back(action{act.button, act.command});
    }
  }

  // Load background
  for (auto&& step : m_conf.get_list<rgba>(bs, "background", {})) {
    m_opts.background_steps.emplace_back(step);
  }

  if (!m_opts.background_steps.empty()) {
    m_opts.background = m_opts.background_steps[0];

    if (m_conf.has(bs, "background")) {
      m_log.warn("Ignoring `%s.background` (overridden by gradient background)", bs);
    }
  } else {
    m_opts.background = color_util::parse(m_conf.get(bs, "background", ""s), m_opts.background);
  }

  // Load foreground
  m_opts.foreground = color_util::parse(m_conf.get(bs, "foreground", ""s), m_opts.foreground);

  // Load over-/underline color and size (warn about deprecated params if used)
  auto line_color = m_conf.get(bs, "line-color", "#f00"s);
  auto line_size = m_conf.get(bs, "line-size", 0);

  m_opts.overline.size = m_conf.get(bs, "overline-size", line_size);
  m_opts.overline.color = color_util::parse(m_conf.get(bs, "overline-color", line_color));
  m_opts.underline.size = m_conf.get(bs, "underline-size", line_size);
  m_opts.underline.color = color_util::parse(m_conf.get(bs, "underline-color", line_color));

  // Load border settings
  auto border_size = m_conf.get(bs, "border-size", 0);
  auto border_color = m_conf.get(bs, "border-color", ""s);

  m_opts.borders.emplace(edge::TOP, border_settings{});
  m_opts.borders[edge::TOP].size = m_conf.deprecated(bs, "border-top", "border-top-size", border_size);
  m_opts.borders[edge::TOP].color = color_util::parse(m_conf.get(bs, "border-top-color", border_color));
  m_opts.borders.emplace(edge::BOTTOM, border_settings{});
  m_opts.borders[edge::BOTTOM].size = m_conf.deprecated(bs, "border-bottom", "border-bottom-size", border_size);
  m_opts.borders[edge::BOTTOM].color = color_util::parse(m_conf.get(bs, "border-bottom-color", border_color));
  m_opts.borders.emplace(edge::LEFT, border_settings{});
  m_opts.borders[edge::LEFT].size = m_conf.deprecated(bs, "border-left", "border-left-size", border_size);
  m_opts.borders[edge::LEFT].color = color_util::parse(m_conf.get(bs, "border-left-color", border_color));
  m_opts.borders.emplace(edge::RIGHT, border_settings{});
  m_opts.borders[edge::RIGHT].size = m_conf.deprecated(bs, "border-right", "border-right-size", border_size);
  m_opts.borders[edge::RIGHT].color = color_util::parse(m_conf.get(bs, "border-right-color", border_color));

  // Load geometry values
  auto w = m_conf.get(m_conf.section(), "width", "100%"s);
  auto h = m_conf.get(m_conf.section(), "height", "24"s);
  auto offsetx = m_conf.get(m_conf.section(), "offset-x", ""s);
  auto offsety = m_conf.get(m_conf.section(), "offset-y", ""s);

  if ((m_opts.size.w = atoi(w.c_str())) && w.find('%') != string::npos) {
    m_opts.size.w = math_util::percentage_to_value<int>(m_opts.size.w, m_opts.monitor->w);
  }
  if ((m_opts.size.h = atoi(h.c_str())) && h.find('%') != string::npos) {
    m_opts.size.h = math_util::percentage_to_value<int>(m_opts.size.h, m_opts.monitor->h);
  }
  if ((m_opts.offset.x = atoi(offsetx.c_str())) != 0 && offsetx.find('%') != string::npos) {
    m_opts.offset.x = math_util::percentage_to_value<int>(m_opts.offset.x, m_opts.monitor->w);
  }
  if ((m_opts.offset.y = atoi(offsety.c_str())) != 0 && offsety.find('%') != string::npos) {
    m_opts.offset.y = math_util::percentage_to_value<int>(m_opts.offset.y, m_opts.monitor->h);
  }

  // Apply offsets
  m_opts.pos.x = m_opts.offset.x + m_opts.monitor->x;
  m_opts.pos.y = m_opts.offset.y + m_opts.monitor->y;
  m_opts.size.h += m_opts.borders[edge::TOP].size;
  m_opts.size.h += m_opts.borders[edge::BOTTOM].size;

  if (m_opts.origin == edge::BOTTOM) {
    m_opts.pos.y = m_opts.monitor->y + m_opts.monitor->h - m_opts.size.h - m_opts.offset.y;
  }

  if (m_opts.size.w <= 0 || m_opts.size.w > m_opts.monitor->w) {
    throw application_error("Resulting bar width is out of bounds (" + to_string(m_opts.size.w) + ")");
  } else if (m_opts.size.h <= 0 || m_opts.size.h > m_opts.monitor->h) {
    throw application_error("Resulting bar height is out of bounds (" + to_string(m_opts.size.h) + ")");
  }

  // m_opts.size.w = math_util::cap<int>(m_opts.size.w, 0, m_opts.monitor->w);
  // m_opts.size.h = math_util::cap<int>(m_opts.size.h, 0, m_opts.monitor->h);

  m_opts.center.y = m_opts.size.h;
  m_opts.center.y -= m_opts.borders[edge::BOTTOM].size;
  m_opts.center.y /= 2;
  m_opts.center.y += m_opts.borders[edge::TOP].size;

  m_opts.center.x = m_opts.size.w;
  m_opts.center.x -= m_opts.borders[edge::RIGHT].size;
  m_opts.center.x /= 2;
  m_opts.center.x += m_opts.borders[edge::LEFT].size;

  m_log.trace("bar: Create renderer");
  m_renderer = renderer::make(m_opts);

  m_log.trace("bar: Attaching sink to registry");
  m_connection.attach_sink(this, SINK_PRIORITY_BAR);

  m_log.info("Bar geometry: %ix%i+%i+%i", m_opts.size.w, m_opts.size.h, m_opts.pos.x, m_opts.pos.y);
  m_opts.window = m_renderer->window();

  // Subscribe to window enter and leave events
  // if we should dim the window
  if (m_opts.dimvalue != 1.0) {
    m_connection.ensure_event_mask(m_opts.window, XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW);
  }

  m_log.info("Bar window: %s", m_connection.id(m_opts.window));
  restack_window();

  m_log.trace("bar: Reconfigure window");
  reconfigure_struts();
  reconfigure_wm_hints();

  m_log.trace("bar: Map window");
  m_connection.map_window_checked(m_opts.window);

  // Reconfigure window position after mapping (required by Openbox)
  // Required by Openbox
  reconfigure_pos();

  m_log.trace("bar: Drawing empty bar");
  m_renderer->begin();
  m_renderer->end();

  m_sig.attach(this);
}

/**
 * Cleanup signal handlers and destroy the bar window
 */
bar::~bar() {
  std::lock_guard<std::mutex> guard(m_mutex);
  m_connection.detach_sink(this, SINK_PRIORITY_BAR);
  m_sig.detach(this);
}

/**
 * Get the bar settings container
 */
const bar_settings bar::settings() const {
  return m_opts;
}

/**
 * Parse input string and redraw the bar window
 *
 * @param data Input string
 * @param force Unless true, do not parse unchanged data
 */
void bar::parse(string&& data, bool force) {
  if (!m_mutex.try_lock()) {
    return;
  }

  std::lock_guard<std::mutex> guard(m_mutex, std::adopt_lock);

  if (force) {
    m_log.trace("bar: Force update");
  } else if (m_opts.shaded) {
    return m_log.trace("bar: Ignoring update (shaded)");
  } else if (data == m_lastinput) {
    return;
  }

  m_lastinput = data;

  m_log.info("Redrawing bar window");
  m_renderer->begin();

  if (m_tray && !m_tray->settings().detached && m_tray->settings().configured_slots) {
    if (m_tray->settings().align == alignment::LEFT) {
      m_renderer->reserve_space(edge::LEFT, m_tray->settings().configured_w);
    } else if (m_tray->settings().align == alignment::RIGHT) {
      m_renderer->reserve_space(edge::RIGHT, m_tray->settings().configured_w);
    }
  }

  try {
    m_parser->parse(settings(), data);
  } catch (const parser_error& err) {
    m_log.err("Failed to parse contents (reason: %s)", err.what());
  }

  m_renderer->end();

  const auto check_dblclicks = [&]() -> bool {
    for (auto&& action : m_renderer->actions()) {
      if (static_cast<uint8_t>(action.button) >= static_cast<uint8_t>(mousebtn::DOUBLE_LEFT)) {
        return true;
      }
    }
    for (auto&& action : m_opts.actions) {
      if (static_cast<uint8_t>(action.button) >= static_cast<uint8_t>(mousebtn::DOUBLE_LEFT)) {
        return true;
      }
    }
    return false;
  };
  m_dblclicks = check_dblclicks();
}

/**
 * Move the bar window above defined sibling
 * in the X window stack
 */
void bar::restack_window() {
  string wm_restack;

  try {
    wm_restack = m_conf.get(m_conf.section(), "wm-restack");
  } catch (const key_error& err) {
    return;
  }

  auto restacked = false;

  if (wm_restack == "bspwm") {
    restacked = bspwm_util::restack_to_root(m_connection, m_opts.monitor, m_opts.window);
#if ENABLE_I3
  } else if (wm_restack == "i3" && m_opts.override_redirect) {
    restacked = i3_util::restack_to_root(m_connection, m_opts.window);
  } else if (wm_restack == "i3" && !m_opts.override_redirect) {
    m_log.warn("Ignoring restack of i3 window (not needed when `override-redirect = false`)");
    wm_restack.clear();
#endif
  } else {
    m_log.warn("Ignoring unsupported wm-restack option '%s'", wm_restack);
    wm_restack.clear();
  }

  if (restacked) {
    m_log.info("Successfully restacked bar window");
  } else if (!wm_restack.empty()) {
    m_log.err("Failed to restack bar window");
  }
}

/**
 * Reconfigure window position
 */
void bar::reconfigure_pos() {
  window win{m_connection, m_opts.window};
  win.reconfigure_pos(m_opts.pos.x, m_opts.pos.y);
}

/**
 * Reconfigure window strut values
 */
void bar::reconfigure_struts() {
  auto geom = m_connection.get_geometry(m_screen->root());
  auto w = m_opts.size.w + m_opts.offset.x;
  auto h = m_opts.size.h + m_opts.offset.y;

  if (m_opts.origin == edge::BOTTOM) {
    h += m_opts.strut.top;
  } else {
    h += m_opts.strut.bottom;
  }

  if (m_opts.origin == edge::BOTTOM && m_opts.monitor->y + m_opts.monitor->h < geom->height) {
    h += geom->height - (m_opts.monitor->y + m_opts.monitor->h);
  } else if (m_opts.origin != edge::BOTTOM) {
    h += m_opts.monitor->y;
  }

  window win{m_connection, m_opts.window};
  win.reconfigure_struts(w, h, m_opts.pos.x, m_opts.origin == edge::BOTTOM);
}

/**
 * Reconfigure window wm hint values
 */
void bar::reconfigure_wm_hints() {
  m_log.trace("bar: Set window WM_NAME");
  xcb_icccm_set_wm_name(m_connection, m_opts.window, XCB_ATOM_STRING, 8, m_opts.wmname.size(), m_opts.wmname.c_str());
  xcb_icccm_set_wm_class(m_connection, m_opts.window, 15, "polybar\0Polybar");

  m_log.trace("bar: Set window _NET_WM_WINDOW_TYPE");
  set_wm_window_type(m_connection, m_opts.window, {_NET_WM_WINDOW_TYPE_DOCK});

  m_log.trace("bar: Set window _NET_WM_STATE");
  set_wm_state(m_connection, m_opts.window, {_NET_WM_STATE_STICKY, _NET_WM_STATE_ABOVE});

  m_log.trace("bar: Set window _NET_WM_DESKTOP");
  set_wm_desktop(m_connection, m_opts.window, 0xFFFFFFFF);

  m_log.trace("bar: Set window _NET_WM_PID");
  set_wm_pid(m_connection, m_opts.window, getpid());
}

/**
 * Broadcast current map state
 */
void bar::broadcast_visibility() {
  auto attr = m_connection.get_window_attributes(m_opts.window);

  if (attr->map_state == XCB_MAP_STATE_UNVIEWABLE) {
    m_sig.emit(visibility_change{move(false)});
  } else if (attr->map_state == XCB_MAP_STATE_UNMAPPED) {
    m_sig.emit(visibility_change{move(false)});
  } else {
    m_sig.emit(visibility_change{move(true)});
  }
}

/**
 * Event handler for XCB_DESTROY_NOTIFY events
 */
void bar::handle(const evt::client_message& evt) {
  if (evt->type == WM_PROTOCOLS && evt->data.data32[0] == WM_DELETE_WINDOW && evt->window == m_opts.window) {
    m_log.err("Bar window has been destroyed, shutting down...");
    m_connection.disconnect();
  }
}

/**
 * Event handler for XCB_DESTROY_NOTIFY events
 */
void bar::handle(const evt::destroy_notify& evt) {
  if (evt->window == m_opts.window) {
    m_connection.disconnect();
  }
}

/**
 * Event handler for XCB_ENTER_NOTIFY events
 *
 * Used to brighten the window by setting the
 * _NET_WM_WINDOW_OPACITY atom value
 */
void bar::handle(const evt::enter_notify&) {
#ifdef DEBUG_SHADED
  if (m_opts.origin == edge::TOP) {
    m_taskqueue->defer_unique("window-hover", 25ms, [&](size_t) { m_sig.emit(signals::ui::unshade_window{}); });
    return;
  }
#endif

  if (m_opts.dimmed) {
    m_taskqueue->defer_unique("window-dim", 25ms, [&](size_t) {
      m_opts.dimmed = false;
      m_sig.emit(dim_window{1.0});
    });
  } else if (m_taskqueue->exist("window-dim")) {
    m_taskqueue->purge("window-dim");
  }
}

/**
 * Event handler for XCB_LEAVE_NOTIFY events
 *
 * Used to dim the window by setting the
 * _NET_WM_WINDOW_OPACITY atom value
 */
void bar::handle(const evt::leave_notify&) {
#ifdef DEBUG_SHADED
  if (m_opts.origin == edge::TOP) {
    m_taskqueue->defer_unique("window-hover", 25ms, [&](size_t) { m_sig.emit(signals::ui::shade_window{}); });
    return;
  }
#endif

  if (!m_opts.dimmed) {
    m_taskqueue->defer_unique("window-dim", 3s, [&](size_t) {
      m_opts.dimmed = true;
      m_sig.emit(dim_window{double(m_opts.dimvalue)});
    });
  }
}

/**
 * Event handler for XCB_BUTTON_PRESS events
 *
 * Used to map mouse clicks to bar actions
 */
void bar::handle(const evt::button_press& evt) {
  if (!m_mutex.try_lock()) {
    return;
  }

  std::lock_guard<std::mutex> guard(m_mutex, std::adopt_lock);

  if (m_buttonpress.deny(evt->time)) {
    return m_log.trace_x("bar: Ignoring button press (throttled)...");
  }

  m_log.trace("bar: Received button press: %i at pos(%i, %i)", evt->detail, evt->event_x, evt->event_y);

  m_buttonpress_btn = static_cast<mousebtn>(evt->detail);
  m_buttonpress_pos = evt->event_x;

  const auto deferred_fn = [&](size_t) {
    for (auto&& action : m_renderer->actions()) {
      if (action.button == m_buttonpress_btn && !action.active && action.test(m_buttonpress_pos)) {
        m_log.trace("Found matching input area");
        m_sig.emit(button_press{string{action.command}});
        return;
      }
    }
    for (auto&& action : m_opts.actions) {
      if (action.button == m_buttonpress_btn && !action.command.empty()) {
        m_log.trace("Found matching fallback handler");
        m_sig.emit(button_press{string{action.command}});
        return;
      }
    }
    m_log.warn("No matching input area found (btn=%i)", static_cast<uint8_t>(m_buttonpress_btn));
  };

  const auto check_double = [&](string&& id, mousebtn&& btn) {
    if (!m_taskqueue->exist(id)) {
      m_doubleclick.event = evt->time;
      m_taskqueue->defer(id, taskqueue::deferred::duration{m_doubleclick.offset}, deferred_fn);
    } else if (m_doubleclick.deny(evt->time)) {
      m_doubleclick.event = 0;
      m_buttonpress_btn = btn;
      m_taskqueue->defer_unique(id, 0ms, deferred_fn);
    }
  };

  // If there are no double click handlers defined we can
  // just by-pass the click timer handling
  if (!m_dblclicks) {
    deferred_fn(0);
  } else if (evt->detail == static_cast<uint8_t>(mousebtn::LEFT)) {
    check_double("buttonpress-left", mousebtn::DOUBLE_LEFT);
  } else if (evt->detail == static_cast<uint8_t>(mousebtn::MIDDLE)) {
    check_double("buttonpress-middle", mousebtn::DOUBLE_MIDDLE);
  } else if (evt->detail == static_cast<uint8_t>(mousebtn::RIGHT)) {
    check_double("buttonpress-right", mousebtn::DOUBLE_RIGHT);
  } else {
    deferred_fn(0);
  }
}

/**
 * Event handler for XCB_EXPOSE events
 *
 * Used to redraw the bar
 */
void bar::handle(const evt::expose& evt) {
  if (evt->window == m_opts.window && evt->count == 0) {
    if (m_tray->settings().running) {
      broadcast_visibility();
    }

    m_log.trace("bar: Received expose event");
    m_renderer->flush();
  }
}

/**
 * Event handler for XCB_PROPERTY_NOTIFY events
 *
 * - Emit events whenever the bar window's
 * visibility gets changed. This allows us to toggle the
 * state of the tray container even though the tray
 * window restacking failed.  Used as a fallback for
 * tedious WM's, like i3.
 *
 * - Track the root pixmap atom to update the
 * pseudo-transparent background when it changes
 */
void bar::handle(const evt::property_notify& evt) {
#ifdef DEBUG_LOGGER_VERBOSE
  string atom_name = m_connection.get_atom_name(evt->atom).name();
  m_log.trace_x("bar: property_notify(%s)", atom_name);
#endif

  if (evt->window == m_opts.window && evt->atom == WM_STATE) {
    broadcast_visibility();
  }
}

bool bar::on(const signals::eventqueue::start&) {
  m_log.trace("bar: Setup tray manager");
  m_tray->setup(static_cast<const bar_settings&>(m_opts));
  broadcast_visibility();
  return true;
}

bool bar::on(const signals::ui::unshade_window&) {
  m_opts.shaded = false;
  m_opts.shade_size.w = m_opts.size.w;
  m_opts.shade_size.h = m_opts.size.h;
  m_opts.shade_pos.x = m_opts.pos.x;
  m_opts.shade_pos.y = m_opts.pos.y;

  double distance{static_cast<double>(m_opts.shade_size.h - m_connection.get_geometry(m_opts.window)->height)};
  double steptime{25.0 / 10.0};
  m_anim_step = distance / steptime / 2.0;

  m_taskqueue->defer_unique("window-shade", 25ms,
      [&](size_t remaining) {
        if (!m_opts.shaded) {
          m_sig.emit(signals::ui::tick{});
        }
        if (!remaining) {
          m_renderer->flush();
        }
        if (m_opts.dimmed) {
          m_opts.dimmed = false;
          m_sig.emit(dim_window{1.0});
        }
      },
      taskqueue::deferred::duration{25ms}, 10U);

  return true;
}

bool bar::on(const signals::ui::shade_window&) {
  taskqueue::deferred::duration offset{2000ms};

  if (!m_opts.shaded && m_opts.shade_size.h != m_opts.size.h) {
    offset = taskqueue::deferred::duration{25ms};
  }

  m_opts.shaded = true;
  m_opts.shade_size.h = 5;
  m_opts.shade_size.w = m_opts.size.w;
  m_opts.shade_pos.x = m_opts.pos.x;
  m_opts.shade_pos.y = m_opts.pos.y;

  if (m_opts.origin == edge::BOTTOM) {
    m_opts.shade_pos.y = m_opts.pos.y + m_opts.size.h - m_opts.shade_size.h;
  }

  double distance{static_cast<double>(m_connection.get_geometry(m_opts.window)->height - m_opts.shade_size.h)};
  double steptime{25.0 / 10.0};
  m_anim_step = distance / steptime / 2.0;

  m_taskqueue->defer_unique("window-shade", 25ms,
      [&](size_t remaining) {
        if (m_opts.shaded) {
          m_sig.emit(signals::ui::tick{});
        }
        if (!remaining) {
          m_renderer->flush();
        }
        if (!m_opts.dimmed) {
          m_opts.dimmed = true;
          m_sig.emit(dim_window{double{m_opts.dimvalue}});
        }
      },
      move(offset), 10U);

  return true;
}

bool bar::on(const signals::ui::tick&) {
  auto geom = m_connection.get_geometry(m_opts.window);
  if (geom->y == m_opts.shade_pos.y && geom->height == m_opts.shade_size.h) {
    return false;
  }

  uint32_t mask{0};
  uint32_t values[7]{0};
  xcb_params_configure_window_t params{};

  if (m_opts.shade_size.h > geom->height) {
    XCB_AUX_ADD_PARAM(&mask, &params, height, static_cast<uint16_t>(geom->height + m_anim_step));
    params.height = std::max(1U, std::min(params.height, static_cast<uint32_t>(m_opts.shade_size.h)));
  } else if (m_opts.shade_size.h < geom->height) {
    XCB_AUX_ADD_PARAM(&mask, &params, height, static_cast<uint16_t>(geom->height - m_anim_step));
    params.height = std::max(1U, std::max(params.height, static_cast<uint32_t>(m_opts.shade_size.h)));
  }

  if (m_opts.shade_pos.y > geom->y) {
    XCB_AUX_ADD_PARAM(&mask, &params, y, static_cast<int16_t>(geom->y + m_anim_step));
    params.y = std::min(params.y, static_cast<int32_t>(m_opts.shade_pos.y));
  } else if (m_opts.shade_pos.y < geom->y) {
    XCB_AUX_ADD_PARAM(&mask, &params, y, static_cast<int16_t>(geom->y - m_anim_step));
    params.y = std::max(params.y, static_cast<int32_t>(m_opts.shade_pos.y));
  }

  connection::pack_values(mask, &params, values);

  m_connection.configure_window(m_opts.window, mask, values);
  m_connection.flush();

  return false;
}

bool bar::on(const signals::ui::dim_window& sig) {
  m_opts.dimmed = sig.cast() != 1.0;
  set_wm_window_opacity(m_connection, m_opts.window, sig.cast() * 0xFFFFFFFF);
  m_connection.flush();
  return false;
}

POLYBAR_NS_END
