// Copy to include/secrets.h (gitignored). BOTH boards must be flashed with the
// same NET_KEY. Generate a fresh one with:   openssl rand -hex 16
// then format as 16 comma-separated 0x.. bytes.
#pragma once

#define NET_KEY  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

// Node ID (1-254). Only used by the node; the base accepts any ID signed with NET_KEY.
#define NODE_ID  1
