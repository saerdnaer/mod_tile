#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>


#include "protocol.h"
#include "render_config.h"
#include "dir_utils.h"

// Build parent directories for the specified file name
// Note: the part following the trailing / is ignored
// e.g. mkdirp("/a/b/foo.png") == shell mkdir -p /a/b
int mkdirp(const char *path) {
    struct stat s;
    char tmp[PATH_MAX];
    char *p;

    strncpy(tmp, path, sizeof(tmp));

    // Look for parent directory
    p = strrchr(tmp, '/');
    if (!p)
        return 0;

    *p = '\0';

    if (!stat(tmp, &s))
        return !S_ISDIR(s.st_mode);
    *p = '/';
    // Walk up the path making sure each element is a directory
    p = tmp;
    if (!*p)
        return 0;
    p++; // Ignore leading /
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (!stat(tmp, &s)) {
                if (!S_ISDIR(s.st_mode)) {
                    fprintf(stderr, "Error, is not a directory: %s\n", tmp);
                    return 1;
                }
            } else if (mkdir(tmp, 0777)) {
                    perror(tmp);
                    return 1;
                }
            *p = '/';
        }
        p++;
    }
    return 0;
}


/* File path hashing. Used by both mod_tile and render daemon
 * The two must both agree on the file layout for meta-tiling
 * to work
 */

void xyz_to_path(char *path, size_t len, const char *tile_dir, struct protocol *tile)
{
#ifdef DIRECTORY_HASH
    // We attempt to cluster the tiles so that a 16x16 square of tiles will be in a single directory
    // Hash stores our 40 bit result of mixing the 20 bits of the x & y co-ordinates
    // 4 bits of x & y are used per byte of output
    unsigned char i, hash[5];
    int x, y;
    x = tile->x;
    y = tile->y;

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
    if ( tile->level == NO_LEVELS )
        snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.png", tile_dir, tile->xmlname, tile->z, hash[4], hash[3], hash[2], hash[1], hash[0]);
    else
        snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.%d.png", tile_dir, tile->xmlname, tile->z, hash[4], hash[3], hash[2], hash[1], hash[0], tile->level);

#else
    if ( tile->level == NO_LEVELS )
        snprintf(path, len, TILE_PATH "/%s/%d/%d/%d.png", tile->xmlname, tile->z, tile->x, tile->y);
    else
        snprintf(path, len, TILE_PATH "/%s/%d/%d/%d.%d.png", tile->xmlname, tile->z, tile->x, tile->y, tile->level);
#endif
    return;
}

int check_xyz(int x, int y, int z)
{
    int oob, limit;

    // Validate tile co-ordinates
    oob = (z < 0 || z > MAX_ZOOM);
    if (!oob) {
         // valid x/y for tiles are 0 ... 2^zoom-1
        limit = (1 << z) - 1;
        oob =  (x < 0 || x > limit || y < 0 || y > limit);
    }

    if (oob)
        fprintf(stderr, "got bad co-ords: x(%d) y(%d) z(%d)\n", x, y, z);

    return oob;
}

int path_to_xyz(const char *path, struct protocol *tile)
{
#ifdef DIRECTORY_HASH
    int i, n, hash[5], x, y;

    n = sscanf(path, HASH_PATH "/%40[^/]/%d/%d/%d/%d/%d/%d.%d", tile->xmlname, &tile->z, &hash[0], &hash[1], &hash[2], &hash[3], &hash[4], &tile->level);
    if (n != 7 && n != 8) {
        fprintf(stderr, "Failed to parse tile path: %s\n", path);
        return 1;
    } else {
        if (n == 7) {
            tile->level = NO_LEVELS;
        }

        x = y = 0;
        for (i=0; i<5; i++) {
            if (hash[i] < 0 || hash[i] > 255) {
                fprintf(stderr, "Failed to parse tile path (invalid %d): %s\n", hash[i], path);
                return 2;
            }
            x <<= 4;
            y <<= 4;
            x |= (hash[i] & 0xf0) >> 4;
            y |= (hash[i] & 0x0f);
        }
        tile->x = x;
        tile->y = y;
        return check_xyz(x, y, tile->z);
    }
#else
    int n;
    n = sscanf(path, TILE_PATH "/%40[^/]/%d/%d/%d.%d", tile->xmlname, tile->&z, tile->&x, tile->&y, tile->&level);
    if (n != 4 && n != 5) {
        fprintf(stderr, "Failed to parse tile path: %s\n", path);
        return 1;
    } else {
        if (n == 4) {
            tile->level = NO_LEVELS;
        }
        return check_xyz(tile->x, tile->y, tile->z);
    }
#endif
}
    
#ifdef METATILE
// Returns the path to the meta-tile and the offset within the meta-tile
int xyz_to_meta(char *path, size_t len, const char *tile_dir, struct protocol *tile)
{
    unsigned char i, hash[5], offset, mask;
    int x, y;
    x = tile->x;
    y = tile->y;

    // Each meta tile winds up in its own file, with several in each leaf directory
    // the .meta tile name is beasd on the sub-tile at (0,0)
    mask = METATILE - 1;
    offset = (x & mask) * METATILE + (y & mask);
    x &= ~mask;
    y &= ~mask;

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
#ifdef DIRECTORY_HASH
    if ( tile->level == NO_LEVELS )
        snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.meta", tile_dir, tile->xmlname, tile->z, hash[4], hash[3], hash[2], hash[1], hash[0]);
    else
        snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.%d.meta", tile_dir, tile->xmlname, tile->z, hash[4], hash[3], hash[2], hash[1], hash[0], tile->level);

#else
    if ( tile->level == NO_LEVELS )
        snprintf(path, len, "%s/%s/%d/%u/%u.meta", tile_dir, tile->xmlname, tile->z, x, y);
    else
        snprintf(path, len, "%s/%s/%d/%u/%u.%d.meta", tile_dir, tile->xmlname, tile->z, x, y, tile->level);
#endif
    return offset;
}
#endif