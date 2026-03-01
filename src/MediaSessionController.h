#pragma once
#include <mutex>
#include <optional>
#include <string>

struct MediaSessionController {
  MediaSessionController();
  ~MediaSessionController();

  // Attempts to (re)discover the Spotify session.
  // Call occasionally (e.g., every few seconds) in case Spotify starts later.
  void Refresh();

  // Returns true if we believe we have a Spotify session handle.
  bool HasSpotifySession() const;

  // Controls
  bool SkipNext();
  bool SkipPrevious();

  // Debug/status text for UI
  std::string StatusText() const;

private:
  struct Impl;
  Impl* impl_;
};
