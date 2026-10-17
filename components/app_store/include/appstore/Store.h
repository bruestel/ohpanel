/*
   Writable storage for the panel configuration: JSON documents, deflated, in an NVS partition of
   their own ("panel", boards/<board>/partitions.csv) -- apart from the settings' nvs, so that a
   configuration saved often never touches the WiFi, the passwords or the keys.

   NVS writes a value whole or not at all: a power cut during a save leaves the previous version.
   It keeps the old value until the new one is complete, so the partition holds two versions and a
   free page -- deflated, a configuration takes a few KB (a large one about 4).

   A document is stored under its slug (the NVS key): validated rather than trusted.
*/
#pragma once

#include <cstddef>
#include <string>

namespace appstore {

/* Opens the partition, formatting it if it has never been used. Safe to call when it is missing
   (a panel still on an older partition table): everything below then reports empty, and a save
   says why it cannot. */
bool begin();
bool available();

/* Lowercase letters, digits and dashes, 1..15 characters (an NVS key). Anything else is refused
   rather than sanitised. */
bool valid_slug(const std::string &slug);

bool exists(const std::string &slug);

/* Empty if there is no such document or it cannot be read -- the caller falls back to a default
   either way. */
std::string load(const std::string &slug);

/* Deflated and stored whole. Refused when the document is larger than kMaxRawBytes or, deflated,
   than kMaxStoredBytes -- the message says how large it is and what fits. */
bool save(const std::string &slug, const std::string &json, std::string *error);

bool erase(const std::string &slug);

struct Usage {
  size_t total = 0;    // bytes of the partition's entries
  size_t used = 0;
  size_t stored = 0;   // the documents, deflated
};
Usage usage();

/* What a save takes at most. Deflated is what counts (what the partition keeps: two versions and a
   free page in 32 KB); the raw limit only keeps a document's parsing within memory. A panel with
   every page and slot used is about 30 KB raw, 4 KB deflated. */
inline constexpr size_t kMaxRawBytes = 128 * 1024;
inline constexpr size_t kMaxStoredBytes = 10 * 1024;

}  // namespace appstore
