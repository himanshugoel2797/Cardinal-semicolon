<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

The disk service manages drivers for all the storage devices connected to the system. The USB system connects into it to provide USB mass storage support. The disk ties into the virtual memory system, handling paging to provide transparent persistence. Programs can request temporary memory or persistent memory, this server takes care of maintaining the illusion of having near limitless ram.

Application memory is stored in versioned data banks. Data banks can be owned by applications. The user interface is simply an object browser, customizeable using object viewers.

File system has superblocks with multiple backup copies.
The file system is log structured.
Consists of 2 data structures:
    - Allocation map - tracks free space for all segments, 1 bit per segment
    - Log structure: 
        - Object store table - list of objects stored on the drive
            - Contains Inodes + B-Tree of file contents
            - Inode metadata:
                - Name
                - Doctype
                - Size
                - Implied content storage format (Inline/B-Tree) based on the size
                - Capability list
        - Object tags - a file that contains the list of tags and their contents, has a special ID
            - Each tag contains a hashmap of objects tagged with the tag
            - Tags can be restricted with capabilities
        - All of these structures are by B-tree
        - All writes are copy-on-write, except for the journal
        - Data can be distributed over multiple disks in a user controlled way, file system does not directly offer an abstraction for multiple disks.
    - File system defragmentation is used every 128 or so segments

Superblock Structure:

    struct CheckpointBlock {
        uint64_t cold_log_top_addr;
        uint64_t default_log_top_addr;
        uint64_t latest_object_id;
        uint64_t imap_head;
    };

    struct Superblock {
        uint32_t checksum;
        char magic[8];
        uint64_t generation;
        uint32_t block_sz;      //In units of bytes
        uint32_t segment_sz;    //In units of blocks
        uint64_t inode_cnt;     //Determines size of imap, should be possible to increase later
        uint64_t flags;
        char label[256];
        uint64_t vol_sz;
        CheckpointBlock blk0;
        CheckpointBlock blk1;
        uint64_t alloc_map[0];
    };

    Possible flags:
        - Volume encryption
        - Boot volume specification
        - Compression flag

    Locations:
        KiB(4)
        MiB(16)
        GiB(64)

Object Store Entry:

    struct ObjectStoreEntry {
        char filename[256];
        uint64_t object_id;
        uint64_t creation_time;
        uint64_t last_opened;
        uint64_t last_written;
        uint16_t flags;
        uint16_t level_cnt;
        uint32_t doctype;
        uint64_t size;
        union {
            uint8_t inline_data[0];
            uint64_t indirection_entries[0];
        }
    };

    Reserved Doctypes:
        0 = Unknown
        1 = Application
        2 = Tag Entry
        3 = Tag Database

    Notes:
        - Applications store their capabilities here, along with the application code.
        - Tags store their hash tables in this objects

Log Structure:

    struct LogHeader {
        uint8_t magic[8];   //'LOGSTART'
        uint64_t timestamp;
        uint64_t block_owner[0];
        uint32_t checksums[0];
    };

    Notes:
        - 1364 blocks per log, because (16384 - 16) / (8 + 4) = 1364 and is a whole number, meaning no space is wasted in the header block.

Defragmentation is slightly different from traditional log file systems, in that cold files are placed into empty blocks near the end of the disk, perhaps based on a hinting API. While hot files stay on the log as is. 