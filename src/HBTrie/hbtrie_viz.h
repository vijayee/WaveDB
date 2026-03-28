//
// Created by victor on 3/27/26.
//

#ifndef WAVEDB_HBTRIE_VIZ_H
#define WAVEDB_HBTRIE_VIZ_H

#include "hbtrie.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Visualize an HBTrie structure as interactive HTML.
 *
 * Generates a JSON representation of the trie and embeds it
 * in a self-contained HTML file with JavaScript viewer.
 *
 * @param trie    HBTrie to visualize
 * @param path    Output file path (e.g., "hbtrie_viz.html")
 * @return 0 on success, -1 on failure
 */
int hbtrie_visualize(hbtrie_t* trie, const char* path);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_HBTRIE_VIZ_H