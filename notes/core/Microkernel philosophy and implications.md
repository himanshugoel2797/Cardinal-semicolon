A microkernel only does that which must be done in the kernel in order to not compromise user level functionality. However historically, exceptions have been made in the favor of performance.

Traditionally, a microkernel does scheduling and ipc. Scheduling being done via scheduler activations, providing a N:M mapping for user threads : kernel threads.  However, this is complicated, so most kernels have moved to a simple 1:1 mapping. This does add the complexity of requiring the kernel to be aware of multithreading and multiple cores.

Virtual address spaces on a 64-bit computer are very large, with the following levels:
4KiB
2MiB
1GiB
512GiB
256TiB

Memory management can be moved to the user space with relative efficiency, but process scheduling remains difficult.

By establishing strict rules regarding memory placement, it's possible to singificantly speed up ipc by putting processes in shared address spaces and simply changing the flags in page tables, which is a much smaller performance cost than a full TLB flush. This can be accomplished by limiting each task to a certain max memory usage (128TiB / 8GiB = 16384 - More than enough processes). Thus, an IPC operation simply makes a set of pages temporarily accessible to another process. This does however still result in some TLB thrashing.


This approach however makes kernel managed SMP significantly more difficult. 
Solutions:

- Perhaps each core has a separate process server and address space? 
    - Increases memory requirements significantly
    - Makes IPC more complex

- Asymmetric Multiprocessing?
    - Cores are resources, used to delegate tasks for fixed amounts of time.
        - Useful for embarassingly parallel problems and job systems
        - Not useful for large numbers of primary tasks
    - Primary core runs the kernel + drivers, other cores run user tasks.
        - Not exactly a microkernel
            - Not a big deal
        - Leaves IPC unsolved
            - Takedowns are expensive
            - Synchronous IPC
                - Block process until target process receives
                    - Zero or single copy
                        - Resolves page sharing concerns
        - Why asymmetric then? Drivers are not different from the other things.
            - Scheduling unsolved
                - Terminal system remains?
                    - Lock other thread's queue, steal a thread
                    - Have a single syscall - IPC