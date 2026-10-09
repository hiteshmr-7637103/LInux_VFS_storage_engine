# Linux Based Virtual File System and Storage Management Engine

## 1. Aim

The aim of this project is to design and implement a Linux based virtual file system and storage management engine in the C programming language. The system emulates the core architecture of a modern file system by operating on a single disk image file through POSIX system calls. It provides a persistent hierarchical namespace, file and directory management, metadata and permission handling, block level storage allocation, free space management, caching, consistency validation, crash recovery, and an interactive command line interface with performance telemetry.

## 2. Overview of Implementation

The project is implemented entirely in C on Linux using POSIX and Linux system calls such as open, read, write, and lseek. The file system is organised in distinct architectural layers, each corresponding to a module in the source tree. These layers are described below in the order in which data flows through them.

### 2.1 Low Level Storage Layer

The disk module treats a single file on the host machine, referred to as the virtual disk image, as a block device. It exposes raw block read and block write operations and is solely responsible for translating a logical block number into a byte offset within the image file.

### 2.2 Superblock and Free Space Management

The bitmap module is responsible for formatting a fresh disk image, a process analogous to the mkfs utility found on real Linux systems. It initialises the superblock, which stores global metadata such as block count, inode count, and the layout of every other region on disk. It also maintains two bitmaps, one tracking free and allocated inodes and the other tracking free and allocated data blocks, and exposes allocation and deallocation primitives for both.

### 2.3 Inode and Metadata Management

The inode module implements the create, read, update, and delete operations for inode records, as well as the logical to physical block mapping scheme. Each inode supports twelve direct block pointers for small files, a single indirect pointer for medium sized files, and a double indirect pointer for large files, mirroring the classical Unix inode design.

### 2.4 Directory Structure and Path Resolution

The vfs module implements the hierarchical namespace. It stores directory entries as fixed size records mapping file names to inode numbers, implements a path resolution parser capable of handling absolute paths, relative paths, the current working directory, parent directory references, and symbolic link expansion, and exposes the complete set of file and directory operations described in section 4.

### 2.5 Caching, Consistency, and Crash Recovery

The cache module implements an in memory block buffer cache using a least recently used eviction policy, reducing the number of direct disk operations required during normal use. The journal module implements a write ahead logging mechanism that protects critical metadata writes, namely the superblock and the two bitmaps, against corruption caused by a crash occurring midway through a write. The fsck module performs a point in time consistency scan of the mounted file system, detecting orphaned inodes, unmarked allocations, and blocks referenced by more than one inode, with an optional repair mode.

### 2.6 Storage Compaction

The defrag module implements a two phase, crash safe block compaction routine. In the first phase every live block reachable from an in use inode is read into memory and assigned a new, contiguous location. In the second phase every relocated block is written to its new location and every inode's pointer fields are rewritten to match, after which the block bitmap and superblock are rebuilt to reflect the compacted layout.

### 2.7 Interactive Command Line Interface

The cli module provides a read eval print loop through which a user issues commands against the mounted file system. It also measures and reports performance characteristics such as input output throughput, directory lookup latency, block cache hit ratio, and allocation efficiency, as detailed in section 5.

## 3. Project File Structure

The project directory is organised as follows.

```
OS_project/
    Makefile
    README.md
    Mini Project Title and Description.docx
    vfs_project_roadmap.md
    include/
        vfs_types.h
        disk.h
        cache.h
        bitmap.h
        inode.h
        vfs.h
        journal.h
        fsck.h
        defrag.h
    src/
        disk.c
        cache.c
        bitmap.c
        inode.c
        vfs.c
        journal.c
        fsck.c
        defrag.c
        cli.c
    bin/
        vfs
    obj/
```

### 3.1 Content of Each File

The Makefile defines the build rules used to compile the project with gcc under the C11 standard with warnings enabled, producing the executable bin/vfs from the object files generated under obj/.

The include/vfs_types.h header defines every on disk data structure used by the file system, namely the superblock structure, the inode structure, and the directory entry structure, together with the constants that describe the disk geometry, such as block size, total block count, and inode count.

The include/disk.h and src/disk.c files define and implement the raw block input and output layer described in section 2.1.

The include/cache.h and src/cache.c files define and implement the in memory block buffer cache described in section 2.5.

The include/bitmap.h and src/bitmap.c files define and implement disk formatting and free space management as described in section 2.2.

The include/inode.h and src/inode.c files define and implement inode record management and block mapping as described in section 2.3.

The include/vfs.h and src/vfs.c files define and implement the hierarchical namespace, path resolution, and every file and directory operation exposed to the command line interface, as described in section 2.4.

The include/journal.h and src/journal.c files define and implement the write ahead logging crash recovery mechanism described in section 2.5.

The include/fsck.h and src/fsck.c files define and implement the consistency validation routine described in section 2.5.

The include/defrag.h and src/defrag.c files define and implement the storage compaction routine described in section 2.6.

The src/cli.c file implements the interactive command line interface described in section 2.7, including every command listed in section 6.

The vfs_project_roadmap.md file records the phased implementation roadmap that guided development of this project, divided into seven phases from architecture definition through to the interactive interface and performance profiling.

## 4. Functionalities of the Virtual File System

The implemented virtual file system provides the following categories of functionality.

1. Persistent hierarchical namespace. The file system persists its entire state, including directories, files, and metadata, inside a single disk image file, and supports an arbitrarily nested directory tree rooted at a single root inode.

2. File and directory management. Files and directories may be created, read, written, renamed, moved, copied, and deleted, including recursive deletion of an entire directory tree.

3. Metadata and permission handling. Every inode records an owner identifier, an owner based read, write, and execute permission mask, a size, a block count, creation and modification timestamps, and a link count. Permission checks are enforced on every operation that reads, writes, or modifies a file or directory, and a process wide default permission mask, referred to as the umask, is applied to the creation of new files and directories.

4. Block level storage allocation. Every file is composed of fixed size data blocks addressed through a direct, single indirect, and double indirect pointer scheme, allowing files to grow from a few bytes to several megabytes without changing the on disk inode layout.

5. Free space management. Two bitmaps track every free and allocated inode and data block on the disk, and every allocation and deallocation operation updates these bitmaps and the superblock atomically through the write ahead logging mechanism.

6. Linking. The file system supports both hard links, in which two directory entries reference the same inode, and symbolic links, in which a dedicated inode stores a target path that is resolved at access time.

7. File caching. An in memory least recently used block cache reduces the number of direct disk reads and writes required during a session, and reports hit, miss, and eviction statistics.

8. Consistency validation and crash recovery. A file system check routine detects and optionally repairs structural inconsistencies, while a separate write ahead logging mechanism protects the superblock and bitmaps against corruption from a crash occurring during a write, and automatically restores the last consistent state when the file system is next mounted.

9. Storage compaction. A defragmentation routine compacts every in use block into a single contiguous region of the disk, reducing fragmentation and maximising the size of the remaining contiguous free space.

10. Host system interoperability. Files may be imported from the host operating system into the virtual file system and exported from the virtual file system back to the host operating system.

11. Performance telemetry. The command line interface reports storage utilisation, input output throughput, directory lookup latency, block cache hit ratio, and allocation efficiency, that is, the proportion of allocated blocks consumed by indirect pointer index blocks rather than file data.

## 5. Performance Metrics

The following performance metrics are measured and reported by the command line interface.

1. Storage utilisation, reported by the df command, expressed as the number and percentage of blocks and inodes in use.

2. Input output throughput, reported after every read and write operation, expressed in kilobytes per second together with the elapsed time of the operation in milliseconds.

3. Directory lookup latency, reported by the stat command, expressed in milliseconds.

4. Block cache performance, reported by the perf command, expressed as the number of cache hits, misses, and evictions together with the resulting hit ratio.

5. Allocation efficiency, reported by the allocstats command, expressed as the number of data blocks against the number of indirect pointer index blocks, together with the resulting overhead ratio.

6. Disk usage, reported by the du command, expressed as the total number of bytes occupied by a file or by a directory tree on disk.

## 6. Commands Supported by the Virtual File System

Once the executable bin/vfs is running, the following commands are accepted at the vfs prompt.

| Command | Description |
|---|---|
| format diskfile | Creates and formats a new virtual disk image. |
| cd path | Changes the current working directory. The default target is the root directory. |
| pwd | Prints the current working directory. |
| ls path | Lists the contents of a directory. The default target is the current working directory. |
| mkdir path | Creates a new directory. The optional flag p also creates any missing parent directories along the path. |
| touch path | Creates a new, empty regular file. |
| write path offset text | Writes the given text into a file starting at the given byte offset. |
| append path text | Appends the given text to the end of a file. |
| cat path | Prints the contents of a file. |
| head path n | Prints the first n lines of a file. The default value of n is ten. |
| tail path n | Prints the last n lines of a file. The default value of n is ten. |
| cp source destination | Copies the contents of a regular file to a new location. |
| mv source destination | Moves or renames a file or a directory. |
| rm path | Deletes a regular file. The optional flag r recursively deletes a directory and its entire contents. |
| rmdir path | Deletes an empty directory. |
| ln target linkpath | Creates a hard link. The optional flag s instead creates a symbolic link. |
| stat path | Displays inode metadata for a file or directory, including type, size, permissions, owner, block count, and link count. |
| size path | Displays the size of a file in bytes. |
| chmod path mode | Sets the owner read, write, and execute permission bits, expressed as a value between zero and seven. |
| chown path uid | Changes the owning user identifier of a file or directory. |
| umask mask | Displays the current default permission mask, or sets it to the given value between zero and seven. |
| import hostfile vfspath | Copies a file from the host operating system into the virtual file system. |
| export vfspath hostfile | Copies a file from the virtual file system out to the host operating system. |
| fsck fix | Runs a consistency check on the mounted file system. The optional argument fix additionally repairs any problems that are found. |
| defrag | Compacts every in use block on the disk into a single contiguous region. |
| df | Displays storage utilisation. The optional flag h displays the result in human readable units. |
| du path | Displays disk usage for a file or directory tree. The optional flag h displays the result in human readable units. |
| allocstats | Displays allocation efficiency statistics. |
| perf | Displays block cache hit, miss, and eviction statistics. |
| help | Displays the list of available commands. |
| exit | Flushes all pending writes, cleanly unmounts the file system, and quits. |

## 7. Building and Running the Project

The project is built using the supplied Makefile. Running the command make from within the project directory compiles every source file and produces the executable at bin/vfs. Running the command make clean removes all generated object files and the executable.

The executable is started by running bin/vfs from a terminal, optionally followed by the path to a disk image file. If no path is supplied, the default file name virtual_disk.img is used. If the specified disk image does not already exist, the format command must be run first to create and initialise it before any other command can be used.
