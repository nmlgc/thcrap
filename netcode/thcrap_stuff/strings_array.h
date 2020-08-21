// TODO: move to thcrap
#pragma once

// Simple (and inefficient) API around an array of NULL-terminated strings

// Allocate a strings array
char **strings_array_create();
// Add an element to the strings array.
// Returns the new array with the new element.
// After this call, the old array becomes invalid.
char **strings_array_add(char **strings_array, const char *str);
// Get the size of the strings array
size_t strings_array_size(char **strings_array);

// Allocate a strings array, and fill it with values.
// Avoids the inefiency of creating an empty array and
// calling strings_array_add repetively (which calls
// new and delete every time).
char **strings_array_create_and_fill(size_t nb_elems, ...);

// Free a strings array and the strings in it
void strings_array_free(char **strings_array);