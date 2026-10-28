/*
   The media page, the favourites and the volume page (Engine::draw_media, draw_favorites, draw_volume_page).
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Cover.h"
#include "appdisplay/Icons.h"
#include "EngineDetail.h"

#include <qrcodegen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace appdisplay {
using namespace detail;

// ------------------------------------------------------------ media page --
//
// What plays filling the page (boards/nspanel/nextion/layout.py MEDIA_*), all objects of
// the design: the art (a large icon, or the cover drawn over it), title,
// artist and album, the controls, the volume pill (the volume is a page of
// its own) and the favourites' star (a page of type "media_favorites").

namespace {
constexpr int kTilesPerSheet = 8;          // the favourites: a button page's tiles
constexpr int64_t kLiveMs = 300;           // a slider moving sends this often at most
constexpr int kCoverScale = 2;             // a cover's pixel, drawn 2 x 2 (Cover.h)
constexpr int kArtRadius = 18;             // the art box's corners (boards/nspanel/nextion/layout.py ART_RADIUS)
constexpr int kCoverFillsPerIdle = 60;     // a cover drawn in parts, a rest between them: a touch is not kept waiting
}  // namespace

std::vector<Engine::Station> Engine::stations_of(const cJSON *page) const {
  std::vector<Station> out;
  const std::string item = str(member(page, "stations"), "item");
  std::vector<appoh::Option> options;
  if (!item.empty() && cb_.options && cb_.options(item, options, false))
    for (const auto &o : options) {
      // the option's label is the station's key; its names come from the metadata, else the key
      const std::string name = o.name.empty() ? o.label : o.name;
      out.push_back({o.value, o.label, o.short_name.empty() ? name : o.short_name, name, o.icon, o.image});
    }
  return out;
}

namespace {
/* A title that is a stream's address or file name ("stream.mp3?cid=...",
   "energy-disneyhits?aggregator=shorty", as a station without metadata
   reports): one long word, or one with "?", "/" or "=" -- nothing to show. */
bool cryptic(const std::string &t) {
  if (t.find(' ') != std::string::npos)
    return false;
  return t.size() >= 24 || t.find_first_of("?/=") != std::string::npos;
}
}  // namespace

/* What a stream is known by: its address without the server and the
   parameters -- both change from play to play (f141.rndfnk.com one time,
   d131 the next; tokens, session ids) -- or, from its title (a stream without
   metadata tells its file name), the name without the parameters. */
std::string Engine::stream_key(const std::string &uri) {
  std::string key = uri.substr(0, uri.find('?'));
  // the scheme and the server go: x-rincon-mp3radio://http://f141.rndfnk.com/ard/... -> /ard/...
  for (size_t at; (at = key.find("://")) != std::string::npos;)
    key.erase(0, at + 3);
  if (const size_t slash = key.find('/'); slash != std::string::npos && uri.find("://") != std::string::npos)
    key.erase(0, slash);
  return key;
}

bool Engine::media_playing(const cJSON *page) const {
  const cJSON *control = member(page, "control");
  return has_state(control) && state_of(control).state == "PLAY";
}

/* The favourites page a media page's star opens; null without (or when it
   is no page of type "media_favorites"). */
const cJSON *Engine::favorites_of(const cJSON *media) const {
  const std::string id = str(media, "favorites");
  const cJSON *page = id.empty() ? nullptr : page_config(id);
  return type_of(page) == PageType::MediaFavorites ? page : nullptr;
}

void Engine::draw_media(const cJSON *page) {
  txt("title", str(page, "title"));
  media_refresh(page);   // the texts first: a field shown draws what it has
  for (const char *obj : {"track", "artist", "album", "art_pic", "art_icon", "prev_icon", "next_icon", "prev_hs",
                          "play_hs", "next_hs"})
    vis(obj, true);
  const bool volume = member(page, "volume") != nullptr;   // the speaker only with a volume
  for (const char *obj : {"volume_pic", "volume_icon", "volume_hs"})
    vis(obj, volume);
  const bool favorites = favorites_of(page) != nullptr;   // the star only with a favourites page
  for (const char *obj : {"fav_pic", "fav_icon", "fav_hs"})
    vis(obj, favorites);
  // one refresh for every state it shows
  for (const char *key : {"control", "volume", "mute", "track", "artist", "album", "uri"})
    bind(member(page, key), [this, page] { media_refresh(page); });
  bind(member(page, "art"), [this, page] { draw_art(page); });   // last: over the fields shown above
}

void Engine::media_refresh(const cJSON *page) {
  const bool playing = media_playing(page);
  std::string track = value_of(member(page, "track")), artist = value_of(member(page, "artist"));
  std::string album = value_of(member(page, "album"));
  const std::string raw_title = cryptic(track) ? track : "";   // a stream's file name: known by it too
  for (std::string *t : {&track, &artist, &album})
    if (cryptic(*t))
      t->clear();
  // a favourite chosen here: its name stays until the player tells a title worth showing -- a stream
  // often tells none, or only its address. With "uri" (what plays, as an address) the station is
  // learnt by the first new address after the tap, without its parameters: its name shows again
  // whenever that plays without a title -- after a pause, a page change, another time
  // -- without "uri", by the title the stream tells (its file name); one heard for two stations
  // (several of a broadcaster's streams are "stream.mp3") tells none
  const std::string id = str(page, "id");
  // what the stream is (its whole address or file name: a new start changes its parameters), and its key
  const std::string raw = member(page, "uri") ? value_of(member(page, "uri")) : raw_title;
  const std::string uri = stream_key(raw);
  if (hint_ && hint_->page == id && !uri.empty() && raw != hint_->uri) {
    if (stations_heard_.size() >= 32)
      stations_heard_.clear();
    const auto known = stations_heard_.find(uri);
    stations_heard_[uri] = known != stations_heard_.end() && known->second != hint_->name ? "" : hint_->name;
    hint_->uri = raw;
  }
  if (hint_ && hint_->page == id && !track.empty() && track != hint_->track)
    hint_.reset();
  std::string station;
  if (hint_ && hint_->page == id)
    station = hint_->name;
  else if (const auto it = stations_heard_.find(uri); track.empty() && !uri.empty() && it != stations_heard_.end())
    station = it->second;   // empty for one heard for two stations
  if (!station.empty()) {
    track = station;
    artist.clear();
    album.clear();
  }
  txt("track", track);
  txt("artist", artist);
  txt("album", album);
  // the art's box and the discs: pictures on the background in use (compose.py art, vol, fav)
  for (const auto &[obj, slot] : {std::pair{"art_pic", "art"}, {"volume_pic", "vol"}, {"fav_pic", "fav"}})
    if (const int pic = picture(slot, "box"); pic >= 0)
      num(obj, "pic", pic);
  // the art: what plays as a large icon on its box -- left alone while a cover is drawn over it
  if (const auto c = drawn_.find("art_pic"); c == drawn_.end() || c->second.empty()) {
    const std::string what = playing || !track.empty() ? "music" : str(page, "icon", "speaker");
    txt("art_icon", icon_glyph(what.c_str()));
    num("art_icon", "pco", theme_.accent);
  }
  // previous, play/pause between them (draw_play), next
  txt("prev_icon", icon_glyph("skip-previous"));
  txt("next_icon", icon_glyph("skip-next"));
  num("prev_icon", "pco", theme_.dim);
  num("next_icon", "pco", theme_.dim);
  draw_play(playing);
  // the volume: the speaker on its disc, crossed out when muted (the number on the volume page)
  if (member(page, "volume")) {
    txt("volume_icon", icon_glyph(on(member(page, "mute")) ? "volume-off" : "volume-high"));
    num("volume_icon", "pco", theme_.text);
  }
  txt("fav_icon", icon_glyph("star"));
  num("fav_icon", "pco", theme_.accent);
}

/* Play/pause: a disc in the theme's accent (disc: by rows, round) in
   the box of play_hs (the manifest's "boxes"), with the symbol on it in
   filled rectangles -- two bars while playing (pause), a triangle while not,
   nudged right to look centred. Exactly centred, whatever the fonts; drawn
   again only when it changes (drawn_, emptied with each page). */
void Engine::draw_play(bool playing) {
  const auto pg = boxes_.find("media");
  if (pg == boxes_.end() || !pg->second.count("play_hs") || note_drawn_)
    return;
  const Box &b = pg->second.at("play_hs");
  const std::string key = std::string(playing ? "pause|" : "play|") + std::to_string(theme_.accent);
  std::string &drawn = drawn_["play"];
  if (drawn == key)
    return;
  drawn = key;
  const int cx = b.x + b.w / 2, cy = b.y + b.h / 2, r = std::min(b.w, b.h) / 2;
  disc(cx, cy, r, theme_.accent);
  // the symbol in proportion to the disc (84 px: bars 9 x 32, 8 apart; a triangle 31 wide, 36 high)
  const int bar_w = r * 9 / 42, bar_h = r * 32 / 42, gap = r * 8 / 42;
  if (playing) {
    const int x0 = cx - (2 * bar_w + gap) / 2, y0 = cy - bar_h / 2;
    fill(x0, y0, bar_w, bar_h, kSymbol);
    fill(x0 + bar_w + gap, y0, bar_w, bar_h, kSymbol);
  } else {
    const int h = r * 36 / 42, w = r * 31 / 42, x0 = cx - w / 2 + w / 10, y0 = cy - h / 2;
    for (int i = 0; i < h; i++) {   // row by row: as wide as the triangle is there
      const double from_middle = std::abs(i - (h - 1) / 2.0) / ((h - 1) / 2.0);
      const int span = static_cast<int>(std::lround(w * (1 - from_middle)));
      if (span > 0)
        fill(x0, y0 + i, span, 1, kSymbol);
    }
  }
}

/* The cover (the "art" item: an address, or the picture itself), drawn over
   the art box once it has been loaded (Callbacks::cover, on_cover) -- last,
   in parts from idle(), from the middle outwards. Without one, or until it
   has come, the icon shows. */
void Engine::draw_art(const cJSON *page) {
  if (shown_ != "media")
    return;
  const cJSON *art = member(page, "art");
  const std::string source = has_state(art) ? state_of(art).state : "";
  const bool wanted = cover_source(source);
  std::string &drawn = drawn_["art_pic"];
  if (!wanted || drawn != source) {
    cover_job_.reset();
    if (!drawn.empty()) {   // the cover shown is not the one wanted: the box and its icon again
      send("ref art_pic");
      send("ref art_icon");
      drawn.clear();
      media_refresh(page);
    }
  }
  if (!wanted || drawn == source)
    return;
  const std::string reveal = settings_.cover_reveal;
  if (cover_ && cover_->source == source && cover_->reveal == reveal) {
    if (cover_->picture && !cover_->picture->rects.empty() && !(cover_job_ && cover_job_->source == source))
      cover_job_ = CoverJob{source, 0};
    return;
  }
  const auto pg = boxes_.find("media");
  if (pg == boxes_.end() || !pg->second.count("art_pic") || (cover_asked_ == source && cover_asked_reveal_ == reveal) ||
      !cb_.cover)
    return;
  cover_asked_ = source;
  cover_asked_reveal_ = reveal;
  cb_.cover(source, pg->second.at("art_pic").w / kCoverScale, kCoverScale, kArtRadius, reveal_of(reveal));
}

void Engine::on_cover(const std::string &source, std::shared_ptr<Cover> cover) {
  cover_ = LoadedCover{source, cover_asked_reveal_, std::move(cover)};
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (page && type_of(page) == PageType::Media)
    draw_art(page);
}

/* A part of the cover being drawn (draw_art), from idle(). */
void Engine::draw_art_step() {
  if (!cover_job_ || note_drawn_ || shown_ != "media")
    return;
  if (!cover_ || cover_->source != cover_job_->source || !cover_->picture) {
    cover_job_.reset();
    return;
  }
  const auto pg = boxes_.find("media");
  if (pg == boxes_.end() || !pg->second.count("art_pic"))
    return;
  const Box &b = pg->second.at("art_pic");
  const Cover &c = *cover_->picture;
  drawn_["art_pic"] = cover_job_->source;   // from now on the icon stays as it is: the cover is over it
  const size_t end = std::min(c.rects.size(), cover_job_->at + kCoverFillsPerIdle);
  const int ox = b.x + (b.w - c.side) / 2, oy = b.y + (b.h - c.side) / 2;
  for (size_t i = cover_job_->at; i < end; i++) {
    const Cover::Rect &r = c.rects[i];
    fill(ox + r.x, oy + r.y, r.w, r.h, r.colour);
  }
  cover_job_->at = end;
  if (end >= c.rects.size())
    cover_job_.reset();
}

/* A station's logo, decoded once; null without one, or for a code that is
   not one (its icon shows then). */
const Logo *Engine::logo_of(const std::string &code) {
  if (code.empty())
    return nullptr;
  if (const auto it = logos_.find(code); it != logos_.end())
    return it->second.w ? &it->second : nullptr;
  if (logos_.size() >= 48)   // catalogues changed often: start afresh
    logos_.clear();
  Logo &logo = logos_[code];   // stays empty for a broken code
  if (decode_logo(code, logo))   // the large rectangles first: the logo is there at once, its edges follow
    std::stable_sort(logo.rects.begin(), logo.rects.end(),
                     [](const Logo::Rect &a, const Logo::Rect &b) { return a.w * a.h > b.w * b.h; });
  return logo.w ? &logo : nullptr;
}

void Engine::queue_logo(const std::string &field, const std::string &code, int under, const std::string &key) {
  for (auto it = logo_queue_.begin(); it != logo_queue_.end(); ++it)
    if (it->field == field) {
      if (it->key == key)
        return;   // on its way already (perhaps half drawn)
      logo_queue_.erase(it);   // what was to come there is out of date
      break;
    }
  const auto drawn = drawn_.find(field);
  if (drawn != drawn_.end() && drawn->second == key)
    return;
  logo_queue_.push_back({shown_, field, key, code, under, 0});
}

/* The logo at the front of the queue (queue_logo), a part of it per call
   from idle() -- a touch is not kept waiting while a page's logos come. */
void Engine::draw_next_logo() {
  if (logo_queue_.empty())
    return;
  LogoJob &job = logo_queue_.front();
  const Logo *logo = shown_ == job.page && !note_drawn_ ? logo_of(job.code) : nullptr;
  if (!logo || !draw_logo(job, *logo))
    logo_queue_.erase(logo_queue_.begin());
}

/* A logo centred in a field (boards/nspanel/nextion/display-manifest.json "boxes"), its
   colours laid on `under`: one fill per rectangle, kLogoFillsPerCall at a
   time; true while there is more of it to draw. */
bool Engine::draw_logo(LogoJob &job, const Logo &logo) {
  constexpr size_t kLogoFillsPerCall = 30;
  const auto page = boxes_.find(shown_);
  if (page == boxes_.end())
    return false;
  const auto box = page->second.find(job.field);
  if (box == page->second.end())
    return false;
  const Box &b = box->second;
  if (job.at == 0) {
    std::string &drawn = drawn_[job.field];
    if (drawn == job.key)
      return false;
    drawn = job.key;
    // the field's icon away first (the field's own colour), then the logo, the large rectangles first
    fill(b.x, b.y, b.w, b.h, job.under);
  }
  const int x0 = b.x + (b.w - logo.w) / 2, y0 = b.y + (b.h - logo.h) / 2;
  const size_t end = std::min(logo.rects.size(), job.at + kLogoFillsPerCall);
  for (size_t i = job.at; i < end; i++) {
    const Logo::Rect &r = logo.rects[i];
    fill(x0 + r.x, y0 + r.y, r.w, r.h, blend565(logo.palette[r.colour], static_cast<uint16_t>(job.under)));
  }
  job.at = end;
  return end < logo.rects.size();
}

void Engine::media_event(const cJSON *page, const std::string &component) {
  if (!cb_.command)
    return;
  const std::string control = str(member(page, "control"), "item");
  if (component == "play" && !control.empty()) {
    cb_.command(control, media_playing(page) ? "PAUSE" : "PLAY");
  } else if ((component == "previous" || component == "next") && !control.empty()) {
    cb_.command(control, component == "next" ? "NEXT" : "PREVIOUS");
  } else if (component == "volume" && member(page, "volume")) {
    show_physical("volpage");   // over this page: closing it comes back
  } else if (component == "favorites" && favorites_of(page)) {
    open(str(page, "favorites"));
  }
}

// -------------------------------------------------------- favourites page --
//
// A station catalogue as tiles on the button pages' design (Engine::draw_buttons):
// its icon or the station's logo above its short name, eight on a sheet,
// more by swiping. A tap sends the station to the play item and goes back --
// to the player, when it opened the page.

void Engine::draw_favorites(const cJSON *page) {
  if (!favorites_asked_ && cb_.options) {   // what openHAB has now, in the background (on_options when it differs)
    favorites_asked_ = true;
    std::vector<appoh::Option> out;
    cb_.options(str(member(page, "stations"), "item"), out, true);
  }
  txt("title", str(page, "title"));
  for (const char *obj : {"title", "close", "dots", "swipe"})
    vis(obj, true);
  const std::vector<Station> list = stations_of(page);
  const int sheets = std::max(1, static_cast<int>((list.size() + kTilesPerSheet - 1) / kTilesPerSheet));
  station_sheet_ = std::clamp(station_sheet_, 0, sheets - 1);
  std::vector<std::string> shown;
  for (int k = 0; k < kTilesPerSheet; k++) {
    const int i = station_sheet_ * kTilesPerSheet + k;
    const std::string n = "tile" + std::to_string(k + 1);
    for (const char *part : {"_icon", "_value"})   // a button's: not here
      vis(n + part, false);
    if (i >= static_cast<int>(list.size())) {
      for (const char *part : {"_hs", "_pic", "_center", "_text"})
        vis(n + part, false);
      continue;
    }
    const Station &s = list[i];
    if (const int tile = picture("button" + std::to_string(k + 1), "off"); tile >= 0)
      num(n + "_pic", "pic", tile);
    // the station's icon, else the radio -- a logo drawn over it later
    const std::string glyph = icon_glyph(s.icon.c_str());
    const std::string field = n + "_center";
    for (const std::string &part : {field, n + "_text"})
      num(part, "bco", kDarkGrey);
    num(field, "pco", theme_.accent);
    num(n + "_text", "pco", kLightGrey);
    txt(field, glyph.empty() ? icon_glyph("radio") : glyph);
    txt(n + "_text", s.short_name);
    for (const std::string &part : {n + "_hs", n + "_pic", field, n + "_text"})
      shown.push_back(part);
  }
  send("ref_stop");
  for (const std::string &obj : shown)
    vis(obj, true);
  send("ref_star");
  // the sheets as dots
  txt("dots", sheets > 1 ? sheet_dots(sheets, station_sheet_) : "");
  // the logos over the icons, one per idle(): the page shows first
  for (int k = 0; k < kTilesPerSheet; k++) {
    const int i = station_sheet_ * kTilesPerSheet + k;
    const std::string field = "tile" + std::to_string(k + 1) + "_center";
    const Logo *logo = i < static_cast<int>(list.size()) ? logo_of(list[i].image) : nullptr;
    if (!logo) {
      drawn_.erase(field);
      continue;
    }
    queue_logo(field, list[i].image, kDarkGrey,
               std::to_string(std::hash<std::string>{}(list[i].image)) + "|" + std::to_string(kDarkGrey));
  }
}

/* The player a favourites page plays on: the one it was opened from, else
   its "player", else the one media page linking it. */
const cJSON *Engine::player_of(const cJSON *favorites) const {
  const std::string id = str(favorites, "id");
  const auto links = [&](const cJSON *pg) { return type_of(pg) == PageType::Media && str(pg, "favorites") == id; };
  if (!history_.empty() && history_.back() != "home")
    if (const cJSON *from = page_config(history_.back()); from && links(from))
      return from;
  if (const cJSON *p = page_config(str(favorites, "player")); p && type_of(p) == PageType::Media)
    return p;
  const cJSON *only = nullptr;
  int count = 0;
  if (const cJSON *pages = member(config_, "pages"); cJSON_IsArray(pages))
    for (const cJSON *pg = pages->child; pg; pg = pg->next)
      if (links(pg)) {
        only = pg;
        count++;
      }
  return count == 1 ? only : nullptr;
}

/* A favourite tapped: the station to its player's play item, the player
   shown -- playing at once, with the station's name until it tells a title. */
void Engine::favorites_event(const cJSON *page, int index) {
  const std::vector<Station> list = stations_of(page);
  const int i = station_sheet_ * kTilesPerSheet + index;
  if (index < 0 || i >= static_cast<int>(list.size()))
    return;
  const cJSON *player = player_of(page);
  const std::string play = str(member(player, "play"), "item");
  if (!player || play.empty() || !cb_.command)
    return;
  cb_.command(play, list[i].uri);
  if (const std::string control = str(member(player, "control"), "item"); !control.empty() && cb_.expect)
    cb_.expect(control, "PLAY");
  const std::string track = value_of(member(player, "track"));
  hint_ = Hint{str(player, "id"), list[i].name, cryptic(track) ? "" : track,
               member(player, "uri") ? value_of(member(player, "uri")) : cryptic(track) ? track : ""};
  const std::string id = str(player, "id");
  if (!history_.empty() && history_.back() == id)
    back();
  else
    open(id, false);   // in place of this page: closing the player goes where this page came from
}

/* The station catalogues of the favourites pages, fetched once openHAB is
   there (a while after it connected: home first) -- and asked again whenever
   a favourites page opens, which shows what it has at once; and the ends of
   the lights' colour temperatures in openHAB, where a page has none of its
   own, ready when the page opens. */

void Engine::draw_volume_page() {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  txt("title", settings_.texts.volume_title);
  txt("minus", icon_glyph("minus"));
  txt("plus", icon_glyph("plus"));
  const auto draw = [this, page] {
    const cJSON *v = member(page, "volume");
    const bool known = has_state(v) && state_of(v).has_numeric && !state_of(v).undefined();
    const int volume = known ? static_cast<int>(state_of(v).numeric + 0.5) : 0;
    if (now_ms_ - volume_moved_ms_ > 1500) {   // not while the finger moves it: its own echo would pull it back
      volume_shown_ = volume;
      num("slider", "val", volume);
      txt("value", known ? std::to_string(volume) + " %" : settings_.texts.undefined);   // unknown: "-"
    }
    // the mute button: the settings page's restart tile, white while muted
    const bool muted = on(member(page, "mute"));
    if (const int off = picture("restart", "off"), onp = picture("restart", "on"); off >= 0 && onp >= 0) {
      num("mute", "pic", muted ? onp : off);
      num("mute", "pic2", muted ? off : onp);
    }
    num("mute", "pco", muted ? kDarkGrey : kWhite);
    num("mute", "pco2", muted ? kWhite : kDarkGrey);
    txt("mute", icon_glyph(muted ? "volume-off" : "volume-high") + "  " + settings_.texts.mute);
    vis("mute", member(page, "mute") != nullptr);
  };
  bind(member(page, "volume"), draw);
  bind(member(page, "mute"), draw);
  draw();
}

void Engine::volume_event(const std::string &component) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  const std::string vol = str(member(page, "volume"), "item"), mute = str(member(page, "mute"), "item");
  if (!cb_.command)
    return;
  if ((component == "minus" || component == "plus") && !vol.empty()) {
    const int v = std::clamp(volume_shown_ + (component == "plus" ? 5 : -5), 0, 100);
    volume_shown_ = v;
    volume_moved_ms_ = now_ms_;
    cb_.command(vol, std::to_string(v));
    num("slider", "val", v);
    txt("value", std::to_string(v) + " %");
  } else if (component == "mute" && !mute.empty()) {
    cb_.command(mute, on(member(page, "mute")) ? "OFF" : "ON");
  }
}

/* The slider moved ("vols": sent every kLiveMs at most, so it is heard) or
   was let go ("volu": the last value). */
void Engine::volume_slider(bool done, int value) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  const std::string vol = str(member(page, "volume"), "item");
  volume_shown_ = std::clamp(value, 0, 100);
  volume_moved_ms_ = now_ms_;
  txt("value", std::to_string(volume_shown_) + " %");
  // while it moves too (every kLiveMs at most), unless the page says "live": false
  const bool live = flag(page, "live", true);
  if ((done || (live && now_ms_ - volume_sent_ms_ >= kLiveMs)) && !vol.empty() && cb_.command) {
    volume_sent_ms_ = now_ms_;
    cb_.command(vol, std::to_string(volume_shown_));
  }
}



}  // namespace appdisplay
