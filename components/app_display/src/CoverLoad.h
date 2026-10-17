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
   (cover_rects); `done` hears of it. A request
   while one is loading replaces the one waiting. */
void cover_load(const std::string &source, int size, int scale, int radius, Reveal reveal);

}  // namespace appdisplay
