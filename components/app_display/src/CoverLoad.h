#pragma once

#include "appdisplay/Cover.h"

#include <functional>
#include <memory>
#include <string>

namespace appdisplay {

/* A cover loaded (null: it could not be), told from the loader's task. */
using CoverDone = std::function<void(const std::string &source, std::shared_ptr<Cover> cover)>;

void cover_begin(CoverDone done);

/* Loads a cover (cover_source) as size x size pixels, each scale px large,
   corners rounded by radius px, in the order `reveal` draws them
   (cover_rects); with fit_w and fit_h the whole picture as large as it fits in that many pixels, its shape kept
   (picture_rects; size then unused). A "#..." at the end of the address is not fetched: a key of the caller's
   (the same picture again). `done` hears of it, with the address as asked. A request
   while one is loading replaces the one waiting. */
void cover_load(const std::string &source, int size, int scale, int radius, Reveal reveal, int fit_w = 0, int fit_h = 0);

}  // namespace appdisplay
