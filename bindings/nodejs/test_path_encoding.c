#include <stdio.h>
#include "../src/HBTrie/hbtrie.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/HBTrie/chunk.h"
#include "../src/Util/allocator.h"

int main() {
    // Create a path for "key0"
    path_t* path1 = path_create();
    identifier_t* id1 = identifier_from_string("key0", 4, 4);
    path_append(path1, id1);
    identifier_destroy(id1);
    
    // Create another path for the same "key0"
    path_t* path2 = path_create();
    identifier_t* id2 = identifier_from_string("key0", 4, 4);
    path_append(path2, id2);
    identifier_destroy(id2);
    
    // Compare the paths
    printf("Path 1 identifiers: %d\n", path1->identifiers.length);
    printf("Path 2 identifiers: %d\n", path2->identifiers.length);
    printf("Path 1 identifier 0 chunks: %d\n", path1->identifiers.data[0]->chunks.length);
    printf("Path 2 identifier 0 chunks: %d\n", path2->identifiers.data[0]->chunks.length);
    
    // Compare chunks
    for (int i = 0; i < path1->identifiers.data[0]->chunks.length; i++) {
        chunk_t* c1 = path1->identifiers.data[0]->chunks.data[i];
        chunk_t* c2 = path2->identifiers.data[0]->chunks.data[i];
        int cmp = chunk_compare(c1, c2);
        printf("Chunk %d compare: %d\n", i, cmp);
    }
    
    path_destroy(path1);
    path_destroy(path2);
    
    return 0;
}
