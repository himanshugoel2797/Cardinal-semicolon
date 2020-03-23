# Syscalls

## Capabilities

Capabilities are opaque objects passed to the kernel
Each capability can define a set of functions it provides access to (in an object oriented manner)
Capabilities are thus constructed via syscalls and can specify their functions
Thus, every kind of object specified below can be implemented as a capability
Provide userspace syscalls for page table management
All capabilities have a 'release' method called when a capability is released
Thus, there is no such thing as a traditional process, 
Continuously running tasks can request a periodic invocation from the kernel
Crashing capabilities error out
Watchdog used instead of a regular timeslice, exit task on watchdog timeout
Yield/tick call performs the watchdog reset followed by a task switch
Syscalls:
  createobj - specify an object template
      - takes an array of function descriptors
          - specifies input params
          - specifies output params
      - returns id of object in the current process's capability table
  invoke - invoke the specified method in the object
  release - release the specified object
  periodic_invoke - request the specified method to be invoked periodically
  watchdog_tick - perform a watchdog reset + task switch

everything is an object of some sort
 - data object
      - desc: contains unstructured, non-executable data
      - interfaces
          - shrink
          - extend
          - read
          - write
          - delete
          - getinfo
 - executable object
      - desc: contains executable code
      - interfaces
          - createprocess
          - update
          - delete
          - getinfo
 - shared object
      - desc: shared memory object
      - interfaces
          - read
          - write
          - getinfo
 - capability share object
      - desc: used to share capabilities between objects
      - interfaces
          - addcap
          - removecap
          - take
          - release
 - process object
      - desc: an executing instance of an executable object
      - interfaces
          - sendmsg
          - receivemsg
          - exit
 - temporary object
      - desc: a temporary object that is never persistent
      - interfaces
          - none - this object is deleted as soon as it is closed and cannot be shared, it can also be cleared at any time
 - domain object
      - desc: an object that can contain other objects under its capability set
      - interfaces
          - enumerate
          - read
          - write
          - delete

common interfaces:
  - create
  - bindcap
  - history
  - revert

first time startup process:
  - startup program with 'startup' capability
  - user environment program instantiated with 'startup' capability for bindcap
      - sendmsg/receivemsg assigned to 'userid' capability
      - exit assigned to 'startup' capability

processes can only obtain capabilities by being given them through another shared capability

initial programs can be read from initrd
    - no built-in persistence from the kernel - processes would request that from appropriate services
    - kernel simply maintains a capability list
        - how to load programs?
            - process create capability?