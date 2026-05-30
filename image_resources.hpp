#pragma once

#include <cstddef>

struct EmbeddedImageResource {
    const unsigned char* data;
    size_t size;
};

EmbeddedImageResource GetNoticeImageResource(int index);
