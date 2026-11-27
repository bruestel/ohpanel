#pragma once

#include <cstdint>

namespace appdisplay {

/* What a front button did, as Tasmota names it: pressed once, twice or three
   times in a row, held, and let go after a hold. */
enum class Gesture { None, Single, Double, Triple, Hold, Clear };

inline const char *gesture_name(Gesture g) {
  switch (g) {
    case Gesture::Single: return "SINGLE";
    case Gesture::Double: return "DOUBLE";
    case Gesture::Triple: return "TRIPLE";
    case Gesture::Hold: return "HOLD";
    case Gesture::Clear: return "CLEAR";
    default: return "";
  }
}

/* What a button listens for (Engine::button_wants): how many presses in a row
   count (1: a press acts at once on release; 2 or 3: it waits that long for the
   next one) and whether a hold counts (else a press up to 2 s is a press). */
struct GestureWants {
  int presses = 1;
  bool hold = false;
};

/* One front button's presses, from its level polled every few milliseconds.
   A press counts on release, after a short debounce, if it was short and the
   other button stayed up (both together belong to the recovery hold). */
class ButtonGesture {
 public:
  static constexpr int64_t kDebounceUs = 40000;
  static constexpr int64_t kLongUs = 2000000;   // without a hold wanted: longer is given up on, switches nothing
  static constexpr int64_t kHoldUs = 800000;    // a hold wanted: from then, while still down
  static constexpr int64_t kGapUs = 350000;     // the next press of a double or triple within this after a release

  Gesture step(bool pressed, bool other, int64_t now, GestureWants w) {
    if (pressed) {
      if (down_since_ == 0) {
        down_since_ = now;
        other_seen_ = false;
        held_ = false;
      }
      other_seen_ = other_seen_ || other;
      if (w.hold && !held_ && !other_seen_ && now - down_since_ >= kHoldUs) {
        held_ = true;
        count_ = 0;   // presses before it are given up
        return Gesture::Hold;
      }
      return Gesture::None;
    }
    if (down_since_ != 0) {
      const int64_t held = now - down_since_;
      down_since_ = 0;
      if (held_) {
        held_ = false;
        return Gesture::Clear;
      }
      if (held <= kDebounceUs)
        return Gesture::None;   // a bounce: a press waiting for the next one still counts
      if (held >= kLongUs || other_seen_) {
        count_ = 0;
        return Gesture::None;
      }
      count_++;
      released_ = now;
      if (count_ >= (w.presses < 1 ? 1 : w.presses > 3 ? 3 : w.presses))
        return take();
      return Gesture::None;
    }
    if (count_ > 0 && now - released_ >= kGapUs)
      return take();
    return Gesture::None;
  }

 private:
  Gesture take() {
    const int n = count_;
    count_ = 0;
    return n >= 3 ? Gesture::Triple : n == 2 ? Gesture::Double : Gesture::Single;
  }

  int64_t down_since_ = 0;
  int64_t released_ = 0;
  bool other_seen_ = false;
  bool held_ = false;
  int count_ = 0;
};

}  // namespace appdisplay
