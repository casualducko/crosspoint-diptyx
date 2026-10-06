#pragma once

#include <string>

struct MarkAsReadResult {
  bool ok = false;     // the book's position was set to the end
  bool moved = false;  // the file was also moved to the Read folder (the Library index is stale)
};

// Library "Mark as Read" for an EPUB: stores the position at the end of the book and then does what finishing the
// book in the reader does: drops it from Recents (Clear Read Books from Recent List) and moves it to /read (Move
// Finished Books to Read Folder), when those settings are on.
MarkAsReadResult markBookAsRead(const std::string& path);
