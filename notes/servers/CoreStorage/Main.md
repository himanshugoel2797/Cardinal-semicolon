<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

The disk service manages drivers for all the storage devices connected to the system. The USB system connects into it to provide USB mass storage support. The disk ties into the virtual memory system, handling paging to provide transparent persistence. Programs can request temporary memory or persistent memory, this server takes care of maintaining the illusion of having near limitless ram.

Application memory is stored in versioned data banks. Data banks can be owned by applications. The user interface is simply an object browser, customizeable using object viewers.

File system has superblocks with multiple backup copies.
Consists of 3 trees:
    - Checksum tree - crc checksums for every block, even for checksum blocks
    - Allocation tree - tracks free space
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
    - A circular buffer for the journal
    - All of these structures are by B-tree
    - All writes are copy-on-write, except for the journal
    - Data can be distributed over multiple disks in a user controlled way, file system does not directly offer an abstraction for multiple disks.

Superblock Structure:

    struct {
        uint32_t checksum;
        char magic[8];
        uint32_t generation;
        uint32_t block_sz;
        uint64_t flag1;
        uint64_t checksum_tree_addr;
        uint64_t allocation_tree_addr;
        uint64_t object_tree_addr;
        uint64_t tag_tree_addr;
        uint64_t latest_object_id;
        char label[256];
        uint64_t vol_sz;
        JournalEntry journal[1024];
        uint8_t rsv_blocks[8 * block_sz];  //space reserved for emergency allocation tree allocations
    };

    Possible flags:
        - Volume encryption
        - Boot volume specification
        - Compression flag

B-Tree structure:

    struct InternalNode {
        uint64_t address;
    };

    struct LeafNode {
        uint32_t offset;
        uint32_t id;    //Specifies the object id pointed to
    };

    struct Header {
        uint32_t checksum
        uint32_t generation;
        uint8_t level;
        uint8_t node_type;
        uint16_t flags;
        uint32_t entry_count;
    };

    Node Types:
        0 = Internal Pointer
        1 = Key-Value Pair

Checksum Entry:

    struct ChecksumEntry {
        uint64_t address;
        uint32_t sz;
        uint32_t checksum;
    };

Allocation Entry:

    struct AllocationEntry {
        uint64_t address;
        uint64_t sz;
    };

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

Journal Entry:

    struct JournalEntry {
        uint16_t operation;
        uint8_t tree_idx;
        uint8_t finished;
        uint32_t src_sz;
        uint64_t src_addr;
    };

    Operations:
        - Insert    //Standard B-Tree insertion operation (COW)
        - Delete    //Standard B-Tree deletion operation (COW)
        - Update    //Standard B-Tree entry update operation (COW)
        - Compact   //Rebuild specified portion of the B-Tree to make it smaller