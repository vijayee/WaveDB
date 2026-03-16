We need to change how the `wal_t` and the `section_t` and `sections_t` interact. Then we need a new primitive called `transaction_id_t`. 
This will work very similiar to the `priority_t` type and will have all the same functions. However, it will have the fields time (time in seconds), 
nanos (time in nanoseconds), and a count field. Whenever a get, put, or delete, Every wal entry will store this transaction_id_t type as apart of the wal entry.
Currently, the `section_t` only stores fixed sized index. The block_size is not a needed field and is leftover from the liboffs implementation.
Remove this field. Instead, we will use the size field to determine the maximum size in byte of an individual section_t. The indexes
will now represent the byte offset from the beginning of the section file. The fragments instead of storing the free indexes, the will now
represent the free byte ranges. We will use them to determine where there is space to store the arbitrary sized data. When data is written
we will first write the `transaction_id_t` that caused this write, next we write the size of the abritrary sized data as a 64bit network encoded integer then write the data. When we read from that index
we first read the transaction_id_t then the first 64 bits to determine the size of the data then the data itself. 

The `sections_t` no longer needs the type field as we don't have need the block_size_e type at all. We also don't need `max_tuple_size`, Instead we will rename this field
`section_concurency` which will represent the minimum number of concurrently in use section it will try to maintain.
We also need to need a new object that stores the oldest wal `transaction_id_t` since the last compaction and the newest `transaction_id_t` stored to disk. We will update this when the HBTrie changes get serialized.
We store this information it to a file called ".range" in the sections folder.