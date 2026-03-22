#include <napi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

struct Point {
  double x;
  double y;
};

struct HotkeyConfig {
  bool requireControl = false;
  bool requireAlt = false;
  bool requireShift = false;
  bool requireCommand = false;
  bool requireShiftForSnap = true;
  std::string key = "X";
};

std::mutex g_callbackMutex;
std::atomic_bool g_listening{false};
std::atomic_bool g_hotkeyActive{false};
std::atomic_bool g_simulating{false};
std::atomic_uint_fast64_t g_mouseEventSequence{1};
Point g_startPoint{0.0, 0.0};
Point g_lastPoint{0.0, 0.0};
HotkeyConfig g_config;

Napi::ThreadSafeFunction g_hotkeyStartCallback;
Napi::ThreadSafeFunction g_hotkeyMoveCallback;
Napi::ThreadSafeFunction g_hotkeyEndCallback;

constexpr int kDragSteps = 24;
constexpr double kPi = 3.14159265358979323846;

Point SnapPointTo45Degrees(Point start, Point target) {
  const double dx = target.x - start.x;
  const double dy = target.y - start.y;
  const double distance = std::sqrt((dx * dx) + (dy * dy));
  if (distance == 0.0) {
    return target;
  }

  const double angle = std::atan2(dy, dx);
  const double snappedAngle = std::round(angle / (kPi / 4.0)) * (kPi / 4.0);
  return Point{
      start.x + (std::cos(snappedAngle) * distance),
      start.y + (std::sin(snappedAngle) * distance)};
}

Point MaybeSnapPoint(Point point, bool shiftDown) {
  if (!g_config.requireShiftForSnap || !shiftDown) {
    return point;
  }

  return SnapPointTo45Degrees(g_startPoint, point);
}

bool MatchesModifierState(bool controlDown, bool altDown, bool shiftDown, bool commandDown) {
  return (!g_config.requireControl || controlDown) &&
         (!g_config.requireAlt || altDown) &&
         (!g_config.requireShift || shiftDown) &&
         (!g_config.requireCommand || commandDown);
}

bool MatchesModifierFlags(uint64_t flags) {
  return MatchesModifierState(
      (flags & kCGEventFlagMaskControl) != 0,
      (flags & kCGEventFlagMaskAlternate) != 0,
      (flags & kCGEventFlagMaskShift) != 0,
      (flags & kCGEventFlagMaskCommand) != 0);
}

bool RequiredModifiersReleased(bool controlDown, bool altDown, bool shiftDown, bool commandDown) {
  return (!g_config.requireControl || !controlDown) &&
         (!g_config.requireAlt || !altDown) &&
         (!g_config.requireShift || !shiftDown) &&
         (!g_config.requireCommand || !commandDown);
}

std::string NormalizeKeyName(const std::string& key) {
  if (key.empty()) {
    return "X";
  }

  std::string normalized;
  normalized.reserve(key.size());
  for (char ch : key) {
    normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

void EmitPoint(Napi::ThreadSafeFunction& callback, Point point) {
  if (!callback) {
    return;
  }

  auto* payload = new Point(point);
  callback.BlockingCall(payload, [](Napi::Env env, Napi::Function jsCallback, Point* data) {
    Napi::Object pointObject = Napi::Object::New(env);
    pointObject.Set("x", Napi::Number::New(env, data->x));
    pointObject.Set("y", Napi::Number::New(env, data->y));
    jsCallback.Call({pointObject});
    delete data;
  });
}

void ReleaseCallbacks() {
  std::lock_guard<std::mutex> lock(g_callbackMutex);

  if (g_hotkeyStartCallback) {
    g_hotkeyStartCallback.Release();
    g_hotkeyStartCallback = Napi::ThreadSafeFunction();
  }

  if (g_hotkeyMoveCallback) {
    g_hotkeyMoveCallback.Release();
    g_hotkeyMoveCallback = Napi::ThreadSafeFunction();
  }

  if (g_hotkeyEndCallback) {
    g_hotkeyEndCallback.Release();
    g_hotkeyEndCallback = Napi::ThreadSafeFunction();
  }
}

Point PointFromValue(const Napi::Value& value) {
  if (!value.IsObject()) {
    throw std::runtime_error("Point must be an object");
  }

  Napi::Object object = value.As<Napi::Object>();
  if (!object.Has("x") || !object.Has("y")) {
    throw std::runtime_error("Point object requires x and y");
  }

  return Point{
      object.Get("x").ToNumber().DoubleValue(),
      object.Get("y").ToNumber().DoubleValue()};
}

#if defined(__APPLE__)

constexpr int64_t kSyntheticTag = 0x534C4401;

CFMachPortRef g_eventTap = nullptr;
CFRunLoopSourceRef g_runLoopSource = nullptr;
CFRunLoopRef g_runLoop = nullptr;
std::thread g_eventTapThread;
std::atomic_bool g_controlDown{false};
std::atomic_bool g_altDown{false};
std::atomic_bool g_shiftDown{false};
std::atomic_bool g_commandDown{false};
std::atomic_bool g_hotkeyKeyDown{false};

Point GetMouseLocation();

uint64_t ManagedModifierMask() {
  uint64_t mask = 0;
  if (g_config.requireControl) {
    mask |= kCGEventFlagMaskControl;
  }
  if (g_config.requireAlt) {
    mask |= kCGEventFlagMaskAlternate;
  }
  if (g_config.requireShift || g_config.requireShiftForSnap) {
    mask |= kCGEventFlagMaskShift;
  }
  if (g_config.requireCommand) {
    mask |= kCGEventFlagMaskCommand;
  }
  return mask;
}

bool ManagedModifiersReleased() {
  return RequiredModifiersReleased(
             g_controlDown.load(),
             g_altDown.load(),
             g_shiftDown.load(),
             g_commandDown.load()) &&
         (!g_config.requireShiftForSnap || !g_shiftDown.load());
}

bool IsManagedModifierKey(CGKeyCode keyCode) {
  if ((keyCode == 59 || keyCode == 62) && g_config.requireControl) {
    return true;
  }
  if ((keyCode == 58 || keyCode == 61) && g_config.requireAlt) {
    return true;
  }
  if ((keyCode == 56 || keyCode == 60) && (g_config.requireShift || g_config.requireShiftForSnap)) {
    return true;
  }
  if ((keyCode == 55 || keyCode == 54) && g_config.requireCommand) {
    return true;
  }
  return false;
}

bool ShouldSwallowManagedModifier(CGKeyCode keyCode, CGEventFlags flags) {
  if (!IsManagedModifierKey(keyCode)) {
    return false;
  }

  return (static_cast<uint64_t>(flags) & ~ManagedModifierMask()) == 0;
}

void FinishHotkeyIfReleased() {
  if (g_hotkeyActive.load() && !g_hotkeyKeyDown.load() && ManagedModifiersReleased()) {
    g_hotkeyActive = false;
    Point endPoint = MaybeSnapPoint(GetMouseLocation(), false);
    g_lastPoint = endPoint;
    EmitPoint(g_hotkeyEndCallback, endPoint);
  }
}

CGKeyCode KeyCodeForString(const std::string& key) {
  const std::string normalized = NormalizeKeyName(key);
  if (normalized == "A") return 0;
  if (normalized == "S") return 1;
  if (normalized == "D") return 2;
  if (normalized == "F") return 3;
  if (normalized == "H") return 4;
  if (normalized == "G") return 5;
  if (normalized == "Z") return 6;
  if (normalized == "X") return 7;
  if (normalized == "C") return 8;
  if (normalized == "V") return 9;
  if (normalized == "B") return 11;
  if (normalized == "Q") return 12;
  if (normalized == "W") return 13;
  if (normalized == "E") return 14;
  if (normalized == "R") return 15;
  if (normalized == "Y") return 16;
  if (normalized == "T") return 17;
  if (normalized == "1") return 18;
  if (normalized == "2") return 19;
  if (normalized == "3") return 20;
  if (normalized == "4") return 21;
  if (normalized == "6") return 22;
  if (normalized == "5") return 23;
  if (normalized == "=") return 24;
  if (normalized == "9") return 25;
  if (normalized == "7") return 26;
  if (normalized == "-") return 27;
  if (normalized == "8") return 28;
  if (normalized == "0") return 29;
  if (normalized == "]") return 30;
  if (normalized == "O") return 31;
  if (normalized == "U") return 32;
  if (normalized == "[") return 33;
  if (normalized == "I") return 34;
  if (normalized == "P") return 35;
  if (normalized == "L") return 37;
  if (normalized == "J") return 38;
  if (normalized == "'") return 39;
  if (normalized == "K") return 40;
  if (normalized == ";") return 41;
  if (normalized == "\\") return 42;
  if (normalized == ",") return 43;
  if (normalized == "/") return 44;
  if (normalized == "N") return 45;
  if (normalized == "M") return 46;
  if (normalized == ".") return 47;
  return 7;
}

Point GetMouseLocation() {
  CGEventRef event = CGEventCreate(nullptr);
  if (!event) {
    return Point{0.0, 0.0};
  }
  CGPoint location = CGEventGetLocation(event);
  CFRelease(event);
  return Point{location.x, location.y};
}

void PostMouseEvent(CGEventType type, CGPoint point, CGPoint previousPoint, int64_t eventNumber) {
  CGEventRef event = CGEventCreateMouseEvent(nullptr, type, point, kCGMouseButtonLeft);
  if (!event) {
    return;
  }

  CGEventSetFlags(event, 0);
  CGEventSetIntegerValueField(event, kCGMouseEventButtonNumber, kCGMouseButtonLeft);
  CGEventSetIntegerValueField(event, kCGMouseEventNumber, eventNumber);
  CGEventSetIntegerValueField(event, kCGMouseEventSubtype, 0);
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kSyntheticTag);
  if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseDragged) {
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
    CGEventSetDoubleValueField(event, kCGMouseEventPressure, 1.0);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, static_cast<int64_t>(std::llround(point.x - previousPoint.x)));
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, static_cast<int64_t>(std::llround(point.y - previousPoint.y)));
  } else if (type == kCGEventLeftMouseUp) {
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
    CGEventSetDoubleValueField(event, kCGMouseEventPressure, 0.0);
  }
  CGEventPost(kCGSessionEventTap, event);
  CFRelease(event);
}

void SimulateDrag(Point start, Point end) {
  g_simulating = true;

  CGPoint startPoint = CGPointMake(start.x, start.y);
  CGPoint endPoint = CGPointMake(end.x, end.y);
  CGPoint previousPoint = startPoint;
  const int64_t eventNumber = static_cast<int64_t>(g_mouseEventSequence.fetch_add(1));

  CGWarpMouseCursorPosition(startPoint);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  PostMouseEvent(kCGEventLeftMouseDown, startPoint, previousPoint, eventNumber);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  for (int step = 1; step <= kDragSteps; ++step) {
    const double t = static_cast<double>(step) / static_cast<double>(kDragSteps);
    CGPoint intermediate = CGPointMake(
        start.x + ((end.x - start.x) * t),
        start.y + ((end.y - start.y) * t));
    PostMouseEvent(kCGEventLeftMouseDragged, intermediate, previousPoint, eventNumber);
    previousPoint = intermediate;
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  PostMouseEvent(kCGEventLeftMouseUp, endPoint, previousPoint, eventNumber);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  g_simulating = false;
}

CGEventRef EventTapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
  if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
    if (g_eventTap) {
      CGEventTapEnable(g_eventTap, true);
    }
    return event;
  }

  if (!g_listening.load()) {
    return event;
  }

  const int64_t sourceTag = CGEventGetIntegerValueField(event, kCGEventSourceUserData);
  if (sourceTag == kSyntheticTag || g_simulating.load()) {
    return event;
  }

  if (type == kCGEventFlagsChanged) {
    const CGKeyCode keyCode = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    const CGEventFlags flags = CGEventGetFlags(event);
    g_controlDown = (flags & kCGEventFlagMaskControl) != 0;
    g_altDown = (flags & kCGEventFlagMaskAlternate) != 0;
    g_shiftDown = (flags & kCGEventFlagMaskShift) != 0;
    g_commandDown = (flags & kCGEventFlagMaskCommand) != 0;

    if (g_hotkeyActive.load()) {
      FinishHotkeyIfReleased();
      if (IsManagedModifierKey(keyCode)) {
        return nullptr;
      }
    }

    if (ShouldSwallowManagedModifier(keyCode, flags)) {
      return nullptr;
    }
  }

  if (type == kCGEventKeyDown) {
    const CGKeyCode keyCode = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    if (keyCode == KeyCodeForString(g_config.key) &&
        MatchesModifierFlags(CGEventGetFlags(event)) &&
        !g_hotkeyActive.exchange(true)) {
      g_hotkeyKeyDown = true;
      g_startPoint = GetMouseLocation();
      g_lastPoint = g_startPoint;
      EmitPoint(g_hotkeyStartCallback, g_startPoint);
      return nullptr;
    }
  }

  if (type == kCGEventMouseMoved && g_hotkeyActive.load()) {
    g_lastPoint = MaybeSnapPoint(GetMouseLocation(), g_shiftDown.load());
    EmitPoint(g_hotkeyMoveCallback, g_lastPoint);
  }

  if (type == kCGEventKeyUp) {
    const CGKeyCode keyCode = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    if (keyCode == KeyCodeForString(g_config.key) && g_hotkeyActive.load()) {
      g_hotkeyKeyDown = false;
      FinishHotkeyIfReleased();
      return nullptr;
    }
  }

  return event;
}

bool StartPlatformListening() {
  if (g_eventTapThread.joinable()) {
    return true;
  }

  std::promise<bool> startedPromise;
  std::future<bool> startedFuture = startedPromise.get_future();

  g_eventTapThread = std::thread([promise = std::move(startedPromise)]() mutable {
      CGEventMask eventMask =
          CGEventMaskBit(kCGEventKeyDown) |
          CGEventMaskBit(kCGEventKeyUp) |
          CGEventMaskBit(kCGEventFlagsChanged) |
          CGEventMaskBit(kCGEventMouseMoved);

      g_eventTap = CGEventTapCreate(
          kCGSessionEventTap,
          kCGHeadInsertEventTap,
          kCGEventTapOptionDefault,
          eventMask,
          EventTapCallback,
          nullptr);

      if (!g_eventTap) {
        promise.set_value(false);
        return;
      }

      promise.set_value(true);

      g_runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_eventTap, 0);
      g_runLoop = CFRunLoopGetCurrent();
      CFRunLoopAddSource(g_runLoop, g_runLoopSource, kCFRunLoopCommonModes);
      CGEventTapEnable(g_eventTap, true);
      CFRunLoopRun();

      CGEventTapEnable(g_eventTap, false);
      if (g_runLoopSource) {
        CFRunLoopRemoveSource(g_runLoop, g_runLoopSource, kCFRunLoopCommonModes);
        CFRelease(g_runLoopSource);
        g_runLoopSource = nullptr;
      }
      if (g_eventTap) {
        CFRelease(g_eventTap);
        g_eventTap = nullptr;
      }
      g_runLoop = nullptr;
  });

  return startedFuture.get();
}

void StopPlatformListening() {
  if (g_runLoop) {
    CFRunLoopStop(g_runLoop);
  }

  if (g_eventTapThread.joinable()) {
    g_eventTapThread.join();
  }
}

bool AreRequiredModifiersReleased() {
  const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
  return RequiredModifiersReleased(
      (flags & kCGEventFlagMaskControl) != 0,
      (flags & kCGEventFlagMaskAlternate) != 0,
      (flags & kCGEventFlagMaskShift) != 0,
      (flags & kCGEventFlagMaskCommand) != 0);
}

#elif defined(_WIN32)

constexpr ULONG_PTR kSyntheticTag = static_cast<ULONG_PTR>(0x534C4401);

HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
DWORD g_hookThreadId = 0;
std::thread g_hookThread;
std::atomic_bool g_controlDown{false};
std::atomic_bool g_altDown{false};
std::atomic_bool g_shiftDown{false};
std::atomic_bool g_commandDown{false};
std::atomic_bool g_hotkeyKeyDown{false};

bool ManagedModifiersReleased() {
  return RequiredModifiersReleased(
             g_controlDown.load(),
             g_altDown.load(),
             g_shiftDown.load(),
             g_commandDown.load()) &&
         (!g_config.requireShiftForSnap || !g_shiftDown.load());
}

void FinishHotkeyIfReleased() {
  if (g_hotkeyActive.load() && !g_hotkeyKeyDown.load() && ManagedModifiersReleased()) {
    g_hotkeyActive = false;
    Point endPoint = MaybeSnapPoint(GetMouseLocation(), false);
    g_lastPoint = endPoint;
    EmitPoint(g_hotkeyEndCallback, endPoint);
  }
}

DWORD VirtualKeyForString(const std::string& key) {
  const std::string normalized = NormalizeKeyName(key);
  if (normalized.empty()) {
    return static_cast<DWORD>('X');
  }

  return static_cast<DWORD>(normalized[0]);
}

Point GetMouseLocation() {
  POINT point;
  GetCursorPos(&point);
  return Point{static_cast<double>(point.x), static_cast<double>(point.y)};
}

LONG NormalizeCoordinate(LONG value, LONG maxValue) {
  if (maxValue <= 1) {
    return 0;
  }
  return static_cast<LONG>(std::llround((static_cast<double>(value) * 65535.0) / static_cast<double>(maxValue - 1)));
}

void SendAbsoluteMouseInput(DWORD flags, LONG x, LONG y) {
  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dx = NormalizeCoordinate(x, GetSystemMetrics(SM_CXSCREEN));
  input.mi.dy = NormalizeCoordinate(y, GetSystemMetrics(SM_CYSCREEN));
  input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;
  input.mi.dwExtraInfo = kSyntheticTag;
  SendInput(1, &input, sizeof(INPUT));
}

void SendDragStep(LONG x, LONG y) {
  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dx = NormalizeCoordinate(x, GetSystemMetrics(SM_CXSCREEN));
  input.mi.dy = NormalizeCoordinate(y, GetSystemMetrics(SM_CYSCREEN));
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE;
  input.mi.dwExtraInfo = kSyntheticTag;
  SendInput(1, &input, sizeof(INPUT));
}

void SimulateDrag(Point start, Point end) {
  g_simulating = true;

  SendAbsoluteMouseInput(MOUSEEVENTF_MOVE, static_cast<LONG>(std::llround(start.x)), static_cast<LONG>(std::llround(start.y)));
  Sleep(8);
  SendAbsoluteMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, static_cast<LONG>(std::llround(start.x)),
                         static_cast<LONG>(std::llround(start.y)));
  Sleep(16);

  for (int step = 1; step <= kDragSteps; ++step) {
    const double t = static_cast<double>(step) / static_cast<double>(kDragSteps);
    const LONG x = static_cast<LONG>(std::llround(start.x + ((end.x - start.x) * t)));
    const LONG y = static_cast<LONG>(std::llround(start.y + ((end.y - start.y) * t)));
    SendDragStep(x, y);
    Sleep(8);
  }

  Sleep(8);
  SendAbsoluteMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTUP, static_cast<LONG>(std::llround(end.x)),
                         static_cast<LONG>(std::llround(end.y)));
  g_simulating = false;
}

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0 || !g_listening.load()) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  const auto* hookData = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
  if (hookData->dwExtraInfo == kSyntheticTag || g_simulating.load()) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  if (wParam == WM_MOUSEMOVE && g_hotkeyActive.load()) {
    g_lastPoint = MaybeSnapPoint(
        Point{static_cast<double>(hookData->pt.x), static_cast<double>(hookData->pt.y)},
        g_shiftDown.load());
    EmitPoint(g_hotkeyMoveCallback, g_lastPoint);
  }

  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0 || !g_listening.load()) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  const auto* hookData = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
  if (hookData->dwExtraInfo == kSyntheticTag) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  const bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
  const bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

  if (hookData->vkCode == VK_LCONTROL || hookData->vkCode == VK_RCONTROL || hookData->vkCode == VK_CONTROL) {
    if (isKeyDown) {
      g_controlDown = true;
    } else if (isKeyUp) {
      g_controlDown = false;
    }
  }

  if (hookData->vkCode == VK_LSHIFT || hookData->vkCode == VK_RSHIFT || hookData->vkCode == VK_SHIFT) {
    if (isKeyDown) {
      g_shiftDown = true;
    } else if (isKeyUp) {
      g_shiftDown = false;
    }
  }

  if (hookData->vkCode == VK_LMENU || hookData->vkCode == VK_RMENU || hookData->vkCode == VK_MENU) {
    if (isKeyDown) {
      g_altDown = true;
    } else if (isKeyUp) {
      g_altDown = false;
    }
  }

  if (hookData->vkCode == VirtualKeyForString(g_config.key) && isKeyDown &&
      MatchesModifierState(g_controlDown.load(), g_altDown.load(), g_shiftDown.load(), g_commandDown.load()) &&
      !g_hotkeyActive.exchange(true)) {
    g_hotkeyKeyDown = true;
    g_startPoint = GetMouseLocation();
    g_lastPoint = g_startPoint;
    EmitPoint(g_hotkeyStartCallback, g_startPoint);
    return 1;
  }

  if (hookData->vkCode == VirtualKeyForString(g_config.key) && isKeyUp && g_hotkeyActive.load()) {
    g_hotkeyKeyDown = false;
    FinishHotkeyIfReleased();
    return 1;
  }

  if (g_hotkeyActive.load()) {
    FinishHotkeyIfReleased();
  }

  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool StartPlatformListening() {
  if (g_hookThread.joinable()) {
    return true;
  }

  std::promise<bool> startedPromise;
  std::future<bool> startedFuture = startedPromise.get_future();

  g_hookThread = std::thread([promise = std::move(startedPromise)]() mutable {
    g_hookThreadId = GetCurrentThreadId();
    HINSTANCE moduleHandle = GetModuleHandle(nullptr);

    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookProc, moduleHandle, 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, moduleHandle, 0);
    promise.set_value(g_keyboardHook != nullptr && g_mouseHook != nullptr);

    MSG message;
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessage(&message);
    }

    if (g_keyboardHook) {
      UnhookWindowsHookEx(g_keyboardHook);
      g_keyboardHook = nullptr;
    }

    if (g_mouseHook) {
      UnhookWindowsHookEx(g_mouseHook);
      g_mouseHook = nullptr;
    }

    g_hookThreadId = 0;
  });

  return startedFuture.get();
}

void StopPlatformListening() {
  if (g_hookThreadId != 0) {
    PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
  }

  if (g_hookThread.joinable()) {
    g_hookThread.join();
  }
}

bool AreRequiredModifiersReleased() {
  const bool controlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
  const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  return RequiredModifiersReleased(controlDown, altDown, shiftDown, false);
}

#else

bool StartPlatformListening() { return false; }

void StopPlatformListening() {}

bool AreRequiredModifiersReleased() { return true; }

#endif

Napi::Value SetCallbacks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "setCallbacks expects an object");
  }

  Napi::Object callbacks = info[0].As<Napi::Object>();
  ReleaseCallbacks();

  if (callbacks.Has("hotkeyStart") && callbacks.Get("hotkeyStart").IsFunction()) {
    g_hotkeyStartCallback = Napi::ThreadSafeFunction::New(
        env,
        callbacks.Get("hotkeyStart").As<Napi::Function>(),
        "hotkeyStart",
        0,
        1);
  }

  if (callbacks.Has("hotkeyMove") && callbacks.Get("hotkeyMove").IsFunction()) {
    g_hotkeyMoveCallback = Napi::ThreadSafeFunction::New(
        env,
        callbacks.Get("hotkeyMove").As<Napi::Function>(),
        "hotkeyMove",
        0,
        1);
  }

  if (callbacks.Has("hotkeyEnd") && callbacks.Get("hotkeyEnd").IsFunction()) {
    g_hotkeyEndCallback = Napi::ThreadSafeFunction::New(
        env,
        callbacks.Get("hotkeyEnd").As<Napi::Function>(),
        "hotkeyEnd",
        0,
        1);
  }

  return env.Undefined();
}

Napi::Value SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "setConfig expects an object");
  }

  const Napi::Object config = info[0].As<Napi::Object>();
  if (config.Has("key") && config.Get("key").IsString()) {
    g_config.key = NormalizeKeyName(config.Get("key").As<Napi::String>().Utf8Value());
  }

  if (config.Has("requireControl") && config.Get("requireControl").IsBoolean()) {
    g_config.requireControl = config.Get("requireControl").As<Napi::Boolean>().Value();
  }

  if (config.Has("requireAlt") && config.Get("requireAlt").IsBoolean()) {
    g_config.requireAlt = config.Get("requireAlt").As<Napi::Boolean>().Value();
  }

  if (config.Has("requireShift") && config.Get("requireShift").IsBoolean()) {
    g_config.requireShift = config.Get("requireShift").As<Napi::Boolean>().Value();
  }

  if (config.Has("requireCommand") && config.Get("requireCommand").IsBoolean()) {
    g_config.requireCommand = config.Get("requireCommand").As<Napi::Boolean>().Value();
  }

  if (config.Has("requireShiftForSnap") && config.Get("requireShiftForSnap").IsBoolean()) {
    g_config.requireShiftForSnap = config.Get("requireShiftForSnap").As<Napi::Boolean>().Value();
  }

  return env.Undefined();
}

Napi::Value StartListening(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (g_listening.exchange(true)) {
    return Napi::Boolean::New(env, true);
  }

  const bool started = StartPlatformListening();
  if (!started) {
    g_listening = false;
  }
  return Napi::Boolean::New(env, started);
}

Napi::Value PerformDrag(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2) {
    throw Napi::TypeError::New(env, "performDrag expects start and end points");
  }

  try {
    const Point startPoint = PointFromValue(info[0]);
    const Point endPoint = PointFromValue(info[1]);
    SimulateDrag(startPoint, endPoint);
  } catch (const std::exception& error) {
    throw Napi::TypeError::New(env, error.what());
  }

  return env.Undefined();
}

Napi::Value AreRequiredModifiersReleasedBinding(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), AreRequiredModifiersReleased());
}

Napi::Value StopListening(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_listening.exchange(false)) {
    return Napi::Boolean::New(env, true);
  }

  g_hotkeyActive = false;
  StopPlatformListening();
  return Napi::Boolean::New(env, true);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("setConfig", Napi::Function::New(env, SetConfig));
  exports.Set("setCallbacks", Napi::Function::New(env, SetCallbacks));
  exports.Set("startListening", Napi::Function::New(env, StartListening));
  exports.Set("areRequiredModifiersReleased", Napi::Function::New(env, AreRequiredModifiersReleasedBinding));
  exports.Set("performDrag", Napi::Function::New(env, PerformDrag));
  exports.Set("stopListening", Napi::Function::New(env, StopListening));
  return exports;
}

}  // namespace

NODE_API_MODULE(straight_drag, Init)
