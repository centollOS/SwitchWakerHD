// Documents picked with Android's file chooser: SDL hands back content:// URIs, which no file API can
// open. The app's Java side (WwhdActivity.copyPickedDocument) copies one to the app's cache. Only the
// Android build defines this (android_documents.cpp).
#pragma once
#include <cstdint>
#include <string>
namespace host::android {
// Copies the document to a new temporary folder under the app's cache, keeping its display name, and
// returns the copy's path; raises with a message for the player (also when it is larger than limit).
std::string copy_picked_document(const std::string& uri, uint64_t limit);
}
