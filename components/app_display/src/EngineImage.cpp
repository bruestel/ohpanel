/*
   The image page (type "image"): one picture as large as the page has room under its title -- an Image item's
   (fetched from openHAB when shown: appoh::rest_state_path), an address an item holds (http(s), a path on openHAB,
   "data:image/..."), or a fixed "url" (a camera's snapshot) -- drawn as filled rectangles like the media page's
   cover (Cover.h, CoverLoad), its shape kept, on the QR page's design: its title and close cross, its code and
   caption left empty. "refresh": seconds after which the picture is fetched again while the page shows; without it
   only when the item changes (an address: by default every 10 s).
*/

#include "appdisplay/Engine.h"

#include "EngineDetail.h"

#include <algorithm>

namespace appdisplay {

using namespace detail;

namespace {

constexpr int kImageMargin = 10;    // the area: this far from the left and right edge of what shows
constexpr int kImageBottom = 32;    // ... and from the bottom: the page dots
constexpr int kImageRadius = 8;     // its corners
constexpr int kImagePixel = 3;      // display px per picture pixel: a few thousand rectangles, 2 to 3 s to draw
constexpr int kImageFillsPerIdle = 60;
constexpr int kUrlRefreshS = 10;    // a fixed address: a camera's snapshot, fetched again this often by default

}  // namespace

/* Where the picture goes: under the title (where the QR page's code begins) to above the page dots. */
Engine::Box Engine::image_area() const {
  int top = 40;
  if (const auto pg = boxes_.find("qr"); pg != boxes_.end())
    if (const auto it = pg->second.find("qr_area"); it != pg->second.end())
      top = it->second.y;
  return {kImageMargin, top, display_size_.visible_w - 2 * kImageMargin, display_size_.h - top - kImageBottom};
}

/* The picture's address: the item's state where it is one (cover_source), else the page's "url"; "" for none. */
std::string Engine::image_source(const cJSON *page) const {
  const cJSON *item = member(page, "item");
  if (cJSON_IsString(item) && item->valuestring[0]) {
    if (!has_state(page))
      return "";
    const appoh::ItemState s = state_of(page);
    return !s.undefined() && cover_source(s.state) ? s.state : "";
  }
  const std::string url = str(page, "url");
  return cover_source(url) ? url : "";
}

int Engine::image_refresh_s(const cJSON *page) const {
  const cJSON *r = member(page, "refresh");
  if (cJSON_IsNumber(r))
    return std::clamp(r->valueint, 0, 86400);
  const cJSON *item = member(page, "item");
  return cJSON_IsString(item) && item->valuestring[0] ? 0 : kUrlRefreshS;
}

/* The page loaded, or its item changed, or it is time to fetch the picture again (image_tick): the picture asked
   for (Callbacks::cover) unless it is drawn already or on its way; drawn in parts from idle() (draw_image_step). */
void Engine::draw_image(const cJSON *page) {
  if (shown_ != "qr")
    return;
  txt("title", str(page, "title"));
  const std::string source = image_source(page);
  txt("caption2", "");
  std::string &drawn = drawn_["image"];
  if (source.empty()) {
    if (!drawn.empty()) {   // the picture is gone: the page anew, empty
      show_physical(shown_);
      return;
    }
    txt("caption1", settings_.texts.undefined);
    return;
  }
  txt("caption1", "");
  // a picture fetched again (refresh) has a key of its own: the same address, another count after "#"
  const std::string key = image_refresh_s(page) > 0 ? source + "#" + std::to_string(image_gen_) : source;
  if (drawn == key)
    return;
  // as the display's settings say (cover_reveal), as the media page's cover -- a picture fetched again too
  const std::string &reveal = settings_.cover_reveal;
  if (cover_ && cover_->source == key) {
    if (cover_->picture && !cover_->picture->rects.empty() && !(cover_job_ && cover_job_->source == key))
      cover_job_ = CoverJob{key, 0};
    return;
  }
  if (cover_asked_ == key || !cb_.cover)
    return;
  cover_asked_ = key;
  cover_asked_reveal_ = reveal;
  const Box a = image_area();
  const int scale = std::clamp(member(page, "pixel") && cJSON_IsNumber(member(page, "pixel"))
                                   ? member(page, "pixel")->valueint : kImagePixel, 2, 6);
  cb_.cover(key, 0, scale, kImageRadius, reveal_of(reveal), a.w / scale, a.h / scale);
}

/* A part of the picture being drawn (draw_image), from idle(); centred in the area. One of another size than the
   one shown: the page anew first (what the old one covered and the new one does not would stay). */
void Engine::draw_image_step() {
  if (!cover_job_ || note_drawn_ || shown_ != "qr")
    return;
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || type_of(page) != PageType::Image)
    return;
  if (!cover_ || cover_->source != cover_job_->source || !cover_->picture) {
    cover_job_.reset();
    return;
  }
  const Cover &c = *cover_->picture;
  const Box a = image_area();
  const Box at{a.x + (a.w - c.w) / 2, a.y + (a.h - c.h) / 2, c.w, c.h};
  std::string &drawn = drawn_["image"];
  if (cover_job_->at == 0) {
    if (!drawn.empty() && (at.x != image_box_.x || at.y != image_box_.y || at.w != image_box_.w ||
                           at.h != image_box_.h)) {
      cover_job_.reset();
      show_physical(shown_);
      return;
    }
    image_box_ = at;
  }
  drawn = cover_job_->source;
  const size_t end = std::min(c.rects.size(), cover_job_->at + kImageFillsPerIdle);
  for (size_t i = cover_job_->at; i < end; i++) {
    const Cover::Rect &r = c.rects[i];
    fill(at.x + r.x, at.y + r.y, r.w, r.h, r.colour);
  }
  cover_job_->at = end;
  if (end >= c.rects.size()) {
    cover_job_.reset();
    image_loaded_ms_ = now_ms_;   // "refresh" counts from the picture shown whole
  }
}

/* "refresh": the picture fetched again that many seconds after the last one was drawn (or could not be fetched:
   tried again then), while the page shows and nothing of it is on its way. */
void Engine::image_tick() {
  if (shown_ != "qr" || cover_job_ || note_drawn_)
    return;
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || type_of(page) != PageType::Image)
    return;
  const int refresh = image_refresh_s(page);
  if (refresh <= 0 || cover_asked_.empty() || !cover_ || cover_->source != cover_asked_ ||
      now_ms_ - image_loaded_ms_ < refresh * 1000LL)
    return;
  image_gen_++;
  draw_image(page);
}

}  // namespace appdisplay
