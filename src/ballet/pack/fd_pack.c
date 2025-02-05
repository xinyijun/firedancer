#define FD_UNALIGNED_ACCESS_STYLE 0
#include "fd_pack.h"
#include "fd_pack_cost.h"
#include "fd_compute_budget_program.h"
#include "fd_pack_bitset.h"
#include "fd_chkdup.h"
#include <math.h> /* for sqrt */
#include <stddef.h> /* for offsetof */
#include "../../disco/metrics/fd_metrics.h"

/* Declare a bunch of helper structs used for pack-internal data
   structures. */

/* fd_pack_ord_txn_t: An fd_txn_p_t with information required to order
   it by priority. */
struct fd_pack_private_ord_txn {
  /* It's important that there be no padding here (asserted below)
     because the code casts back and forth from pointers to this element
     to pointers to the whole struct. */
  fd_txn_p_t   txn[1];

  /* Since this struct can be in one of several trees, it's helpful to
     store which tree.  This should be one of the FD_ORD_TXN_ROOT_*
     values. */
  int root;

  /* Each transaction is inserted with an expiration "time."  This code
     doesn't care about the units (blocks, rdtsc tick, ns, etc.), and
     doesn't require transactions to be inserted in expiration date
     order. */
  ulong expires_at;
  /* expq_idx: When this object is part of one of the treaps, it's
     also in the expiration priority queue.  This field (which is
     manipulated behind the scenes by the fd_prq code) stores where so
     that if we delete this transaction, we can also delete it from the
     expiration priority queue. */
  ulong expq_idx;

  /* We want rewards*compute_est to fit in a ulong so that r1/c1 < r2/c2 can be
     computed as r1*c2 < r2*c1, with the product fitting in a ulong.
     compute_est has a small natural limit of mid-20 bits. rewards doesn't have
     a natural limit, so there is some argument to be made for raising the
     limit for rewards to 40ish bits. The struct has better packing with
     uint/uint though. */
  uint                __attribute__((aligned(64))) /* We want the treap fields and the bitsets
                                                       to be on the same double cache line pair */
               rewards;     /* in Lamports */
  uint         compute_est; /* in compute units */

  /* The treap fields */
  ushort left;
  ushort right;
  ushort parent;
  ushort prio;
  ushort prev;
  ushort next;

  FD_PACK_BITSET_DECLARE( rw_bitset ); /* all accts this txn references */
  FD_PACK_BITSET_DECLARE(  w_bitset ); /* accts this txn write-locks    */

};
typedef struct fd_pack_private_ord_txn fd_pack_ord_txn_t;

/* What we want is that the payload starts at byte 0 of
   fd_pack_ord_txn_t so that the trick with the signature map works
   properly.  GCC and Clang seem to disagree on the rules of offsetof.
   */
FD_STATIC_ASSERT( offsetof( fd_pack_ord_txn_t, txn          )==0UL, fd_pack_ord_txn_t );
#if FD_USING_CLANG
FD_STATIC_ASSERT( offsetof( fd_txn_p_t,             payload )==0UL, fd_pack_ord_txn_t );
#else
FD_STATIC_ASSERT( offsetof( fd_pack_ord_txn_t, txn->payload )==0UL, fd_pack_ord_txn_t );
#endif

#define FD_ORD_TXN_ROOT_FREE            0
#define FD_ORD_TXN_ROOT_PENDING         1
#define FD_ORD_TXN_ROOT_PENDING_VOTE    2

#define FD_PACK_IN_USE_WRITABLE    (0x8000000000000000UL)
#define FD_PACK_IN_USE_BIT_CLEARED (0x4000000000000000UL)

/* Each non-empty microblock we schedule also has an overhead of 48
   bytes that counts towards shed limits.  That comes from the 32 byte
   hash, the hash count (8 bytes) and the transaction count (8 bytes).
   We don't have to pay this overhead if the microblock is empty, since
   those microblocks get dropped. */
#define MICROBLOCK_DATA_OVERHEAD 48UL

/* Keep track of accounts that are written to in each block so that we
   can reset the writer costs to 0.  If the number of accounts that are
   written to is above or equal to this, we'll just clear the whole
   writer cost map instead of only removing the elements we increased. */
#define DEFAULT_WRITTEN_LIST_MAX 16384UL

/* fd_pack_addr_use_t: Used for two distinct purposes:
    -  to record that an address is in use and can't be used again until
         certain microblocks finish execution
    -  to keep track of the cost of all transactions that write to the
         specified account.
   Making these separate structs might make it more clear, but then
   they'd have identical shape and result in two fd_map_dynamic sets of
   functions with identical code.  It doesn't seem like the compiler is
   very good at merging code like that, so in order to reduce code
   bloat, we'll just combine them. */
struct fd_pack_private_addr_use_record {
  fd_acct_addr_t key; /* account address */
  union {
    ulong          in_use_by;  /* Bitmask indicating which banks */
    ulong          total_cost; /* In cost units/CUs */
  };
};
typedef struct fd_pack_private_addr_use_record fd_pack_addr_use_t;


/* fd_pack_sig_to_entry_t: An element of an fd_map that maps the first
   transaction signature to the corresponding fd_pack_ord_txn_t so that
   pending transactions can be deleted by signature.  Note: this
   implicitly relies on the fact that for Solana transactions the
   signature_offset is always 1.  If that fact changes, this will need
   to become a real struct. */
struct fd_pack_sig_to_txn {
  fd_ed25519_sig_t const * key;
};
typedef struct fd_pack_sig_to_txn fd_pack_sig_to_txn_t;

/* fd_pack_expq_t: An element of an fd_prq to sort the transactions by
   timeout.  This structure has several invariants for entries
   corresponding to pending transactions:
     expires_at == txn->expires_at
     txn->exp_prq_idx is the index of this structure
   Notice that prq is an array-based heap, which means the indexes of
   elements change.  The PRQ_TMP_ST macro is hijacked to keep that
   invariant up to date.

   Note: this could be easier if fd_heap supported deleting from the
   middle, but that's not possible with the current design of fd_heap,
   which omits a parent pointer for improved performance. */
struct fd_pack_expq {
  ulong               expires_at;
  fd_pack_ord_txn_t * txn;
};
typedef struct fd_pack_expq fd_pack_expq_t;


/* fd_pack_bitset_acct_mapping_t: An element of an fd_map_dynamic that
   maps an account address to the number of transactions that are
   referencing it and the bit that is reserved to indicate it in the
   bitset, if any. */
struct fd_pack_bitset_acct_mapping {
  fd_acct_addr_t key; /* account address */
  ulong          ref_cnt;

  /* first_instance and first_instance_was_write are only valid when
     bit==FD_PACK_BITSET_FIRST_INSTANCE, which is set when ref_cnt
     transitions from 0 to 1.  These just exist to implement the
     optimization that accounts referenced a single time aren't
     allocated a bit, but this seems to be an important optimization. */
  fd_pack_ord_txn_t * first_instance;
  int                 first_instance_was_write;

  /* bit is in [0, FD_PACK_BITSET_MAX) U
     { FD_PACK_BITSET_FIRST_INSTANCE, FD_PACK_BITSET_SLOWPATH }. */
  ushort              bit;
};
typedef struct fd_pack_bitset_acct_mapping fd_pack_bitset_acct_mapping_t;

/* Table of special addresses that are not allowed to be written to.  We
   immediately reject and refuse to pack any transaction that tries to
   write to one of these accounts.  Because we reject any writes to any
   of these accounts, we actually don't need to track reads of them
   either.  This is nice, because fd_map_dynamic requires a null address
   that we promise never to insert.  The zero address is a sysvar, so
   now we meet that part of the fd_map_dynamic contract. */
#define MAP_PERFECT_NAME      fd_pack_unwritable
#define MAP_PERFECT_LG_TBL_SZ 5
#define MAP_PERFECT_T         fd_acct_addr_t
#define MAP_PERFECT_HASH_C    1402126759U
#define MAP_PERFECT_KEY       b
#define MAP_PERFECT_KEY_T     fd_acct_addr_t const *
#define MAP_PERFECT_ZERO_KEY  (0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0)
#define MAP_PERFECT_COMPLEX_KEY 1
#define MAP_PERFECT_KEYS_EQUAL(k1,k2) (!memcmp( (k1), (k2), 32UL ))

#define PERFECT_HASH( u ) (((MAP_PERFECT_HASH_C*(u))>>27)&0x1FU)

#define MAP_PERFECT_HASH_PP( a00,a01,a02,a03,a04,a05,a06,a07,a08,a09,a10,a11,a12,a13,a14,a15, \
                             a16,a17,a18,a19,a20,a21,a22,a23,a24,a25,a26,a27,a28,a29,a30,a31) \
                                          PERFECT_HASH( (a08 | (a09<<8) | (a10<<16) | (a11<<24)) )
#define MAP_PERFECT_HASH_R( ptr ) PERFECT_HASH( fd_uint_load_4( (uchar const *)ptr->b + 8UL ) )

/* This list is a superset of what Lab's is_builtin_key_or_sysvar checks. */
/* Sysvars */
#define MAP_PERFECT_0  ( SYSVAR_CLOCK_ID          ),
#define MAP_PERFECT_1  ( SYSVAR_EPOCH_SCHED_ID    ),
#define MAP_PERFECT_2  ( SYSVAR_FEES_ID           ),
#define MAP_PERFECT_3  ( SYSVAR_RECENT_BLKHASH_ID ),
#define MAP_PERFECT_4  ( SYSVAR_RENT_ID           ),
#define MAP_PERFECT_5  ( SYSVAR_REWARDS_ID        ),
#define MAP_PERFECT_6  ( SYSVAR_SLOT_HASHES_ID    ),
#define MAP_PERFECT_7  ( SYSVAR_SLOT_HIST_ID      ),
#define MAP_PERFECT_8  ( SYSVAR_STAKE_HIST_ID     ),
#define MAP_PERFECT_9  ( SYSVAR_INSTRUCTIONS_ID   ),
#define MAP_PERFECT_10 ( SYSVAR_EPOCH_REWARDS_ID  ),
#define MAP_PERFECT_11 ( SYSVAR_LAST_RESTART_ID   ),
/* Programs */
#define MAP_PERFECT_12 ( CONFIG_PROG_ID           ),
#define MAP_PERFECT_13 ( FEATURE_ID               ),
#define MAP_PERFECT_14 ( NATIVE_LOADER_ID         ),
#define MAP_PERFECT_15 ( STAKE_PROG_ID            ),
#define MAP_PERFECT_16 ( STAKE_CONFIG_PROG_ID     ),
#define MAP_PERFECT_17 ( VOTE_PROG_ID             ),
#define MAP_PERFECT_18 ( SYS_PROG_ID              ), /* Do not remove. See above. */
#define MAP_PERFECT_19 ( BPF_LOADER_1_PROG_ID     ),
#define MAP_PERFECT_20 ( BPF_LOADER_2_PROG_ID     ),
#define MAP_PERFECT_21 ( BPF_UPGRADEABLE_PROG_ID  ),
/* Extras */
#define MAP_PERFECT_22 ( ED25519_SV_PROG_ID       ),
#define MAP_PERFECT_23 ( KECCAK_SECP_PROG_ID      ),
#define MAP_PERFECT_24 ( COMPUTE_BUDGET_PROG_ID   ),
#define MAP_PERFECT_25 ( ADDR_LUT_PROG_ID         ),
#define MAP_PERFECT_26 ( NATIVE_MINT_ID           ),
#define MAP_PERFECT_27 ( TOKEN_PROG_ID            ),
#define MAP_PERFECT_28 ( SYSVAR_PROG_ID           ),

#include "../../util/tmpl/fd_map_perfect.c"


/* Returns 1 if x.rewards/x.compute < y.rewards/y.compute. Not robust. */
#define COMPARE_WORSE(x,y) ( ((ulong)((x)->rewards)*(ulong)((y)->compute_est)) < ((ulong)((y)->rewards)*(ulong)((x)->compute_est)) )

/* Declare all the data structures */


/* Define the big max-"heap" that we pull transactions off to schedule.
   The priority is given by reward/compute.  We may want to add in some
   additional terms at a later point.  In order to cheaply remove nodes,
   we actually use a treap.  */
#define POOL_NAME       trp_pool
#define POOL_T          fd_pack_ord_txn_t
#define POOL_IDX_T      ushort
#define POOL_NEXT       parent
#include "../../util/tmpl/fd_pool.c"

#define TREAP_T         fd_pack_ord_txn_t
#define TREAP_NAME      treap
#define TREAP_QUERY_T   void *                                         /* We don't use query ... */
#define TREAP_CMP(a,b)  (__extension__({ (void)(a); (void)(b); -1; })) /* which means we don't need to give a real
                                                                          implementation to cmp either */
#define TREAP_IDX_T     ushort
#define TREAP_OPTIMIZE_ITERATION 1
#define TREAP_LT        COMPARE_WORSE
#include "../../util/tmpl/fd_treap.c"


/* Define a strange map where key and value are kind of the same
   variable.  Essentially, it maps the contents to which the pointer
   points to the value of the pointer. */
#define MAP_NAME              sig2txn
#define MAP_T                 fd_pack_sig_to_txn_t
#define MAP_KEY_T             fd_ed25519_sig_t const *
#define MAP_KEY_NULL          NULL
#define MAP_KEY_INVAL(k)      !(k)
#define MAP_MEMOIZE           0
#define MAP_KEY_EQUAL(k0,k1)  (((!!(k0))&(!!(k1)))&&(!memcmp((k0),(k1), FD_TXN_SIGNATURE_SZ)))
#define MAP_KEY_EQUAL_IS_SLOW 1
#define MAP_KEY_HASH(key)     fd_uint_load_4( (key) ) /* first 4 bytes of signature */
#include "../../util/tmpl/fd_map_dynamic.c"


static const fd_acct_addr_t null_addr = { 0 };

#define MAP_NAME              acct_uses
#define MAP_T                 fd_pack_addr_use_t
#define MAP_KEY_T             fd_acct_addr_t
#define MAP_KEY_NULL          null_addr
#define MAP_KEY_INVAL(k)      MAP_KEY_EQUAL(k, null_addr)
#define MAP_KEY_EQUAL(k0,k1)  (!memcmp((k0).b,(k1).b, FD_TXN_ACCT_ADDR_SZ))
#define MAP_KEY_EQUAL_IS_SLOW 1
#define MAP_MEMOIZE           0
#define MAP_KEY_HASH(key)     ((uint)fd_ulong_hash( fd_ulong_load_8( (key).b ) ))
#include "../../util/tmpl/fd_map_dynamic.c"


#define MAP_NAME              bitset_map
#define MAP_T                 fd_pack_bitset_acct_mapping_t
#define MAP_KEY_T             fd_acct_addr_t
#define MAP_KEY_NULL          null_addr
#define MAP_KEY_INVAL(k)      MAP_KEY_EQUAL(k, null_addr)
#define MAP_KEY_EQUAL(k0,k1)  (!memcmp((k0).b,(k1).b, FD_TXN_ACCT_ADDR_SZ))
#define MAP_KEY_EQUAL_IS_SLOW 1
#define MAP_MEMOIZE           0
#define MAP_KEY_HASH(key)     ((uint)fd_ulong_hash( fd_ulong_load_8( (key).b ) ))
#include "../../util/tmpl/fd_map_dynamic.c"


/* Since transactions can also expire, we also maintain a parallel
   priority queue.  This means elements are simultaneously part of the
   treap (ordered by priority) and the expiration queue (ordered by
   expiration).  It's tempting to use the priority field of the treap
   for this purpose, but that can result in degenerate treaps in some
   cases. */
#define PRQ_NAME             expq
#define PRQ_T                fd_pack_expq_t
#define PRQ_TIMEOUT_T        ulong
#define PRQ_TIMEOUT          expires_at
#define PRQ_TMP_ST(p,t)      do {                                   \
                               (p)[0] = (t);                        \
                               t.txn->expq_idx = (ulong)((p)-heap); \
                             } while( 0 )
#include "../../util/tmpl/fd_prq.c"

/* Finally, we can now declare the main pack data structure */
struct fd_pack_private {
  ulong      pack_depth;
  ulong      bank_tile_cnt;

  fd_pack_limits_t lim[1];

  ulong      pending_txn_cnt;
  ulong      microblock_cnt; /* How many microblocks have we
                                generated in this block? */
  ulong      data_bytes_consumed; /* How much data is in this block so
                                     far ? */
  fd_rng_t * rng;

  ulong      cumulative_block_cost;
  ulong      cumulative_vote_cost;

  /* expire_before: Any transactions with expires_at strictly less than
     the current expire_before are removed from the available pending
     transaction.  Here, "expire" is used as a verb: cause all
     transactions before this time to expire. */
  ulong      expire_before;

  /* outstanding_microblock_mask: a bitmask indicating which banking
     tiles have outstanding microblocks, i.e. fd_pack has generated a
     microblock for that banking tile and the banking tile has not yet
     notified fd_pack that it has completed it. */
  ulong      outstanding_microblock_mask;

  /* The actual footprint for the pool and maps is allocated
     in the same order in which they are declared immediately following
     the struct.  I.e. these pointers point to memory not far after the
     struct.  The trees are just pointers into the pool so don't take up
     more space. */

  fd_pack_ord_txn_t * pool;

  /* Treaps (sorted by priority) of pending transactions.  We store the
     pending simple votes separately. */
  treap_t pending[1];
  treap_t pending_votes[1];

  /* expiration_q: At the same time that a transaction is in exactly one
     of the above treaps, it is also in the expiration queue, sorted by
     its expiration time.  This enables deleting all transactions that
     have expired, regardless of which treap they are in. */
  fd_pack_expq_t * expiration_q;

  /* acct_in_use: Map from account address to bitmask indicating which
     bank tiles are using the account and whether that use is read or
     write (msb). */
  fd_pack_addr_use_t   * acct_in_use;

  /* bitset_{w, rw}_in_use stores a subset of the information in
     acct_in_use using the compressed set format explained at the top of
     this file.  rw_in_use stores accounts in use for read or write
     while w_in_use stores only those in use for write. */
  FD_PACK_BITSET_DECLARE( bitset_rw_in_use );
  FD_PACK_BITSET_DECLARE( bitset_w_in_use  );

  /* writer_costs: Map from account addresses to the sum of costs of
     transactions that write to the account.  Used for enforcing limits
     on the max write cost per account per block. */
  fd_pack_addr_use_t   * writer_costs;

  /* At the end of every slot, we have to clear out writer_costs.  The
     map is large, but typically very sparsely populated.  As an
     optimization, we keep track of the elements of the map that we've
     actually used, up to a maximum.  If we use more than the maximum,
     we revert to the old way of just clearing the whole map.

     written_list indexed [0, written_list_cnt).
     written_list_cnt in  [0, written_list_max).

     written_list_cnt==written_list_max-1 means that the list may be
     incomplete and should be ignored. */
  fd_pack_addr_use_t * * written_list;
  ulong                  written_list_cnt;
  ulong                  written_list_max;


  fd_pack_sig_to_txn_t * signature_map; /* Stores pointers into pool for deleting by signature */

  /* use_by_bank: An array of size (max_txn_per_microblock *
     FD_TXN_ACCT_ADDR_MAX) for each banking tile.  Only the MSB of
     in_use_by is relevant.  Addressed use_by_bank[i][j] where i is in
     [0, bank_tile_cnt) and j is in [0, use_by_bank_cnt[i]).  Used
     mostly for clearing the proper bits of acct_in_use when a
     microblock finishes. */
  fd_pack_addr_use_t * use_by_bank    [ FD_PACK_MAX_BANK_TILES ];
  ulong                use_by_bank_cnt[ FD_PACK_MAX_BANK_TILES ];

  fd_histf_t txn_per_microblock [ 1 ];
  fd_histf_t vote_per_microblock[ 1 ];

  fd_histf_t scheduled_cus_per_block[ 1 ];
  fd_histf_t rebated_cus_per_block  [ 1 ];
  fd_histf_t net_cus_per_block      [ 1 ];
  ulong      cumulative_rebated_cus;

  /* bitset_avail: a stack of which bits are not currently reserved and
     can be used to represent an account address.
     Indexed [0, bitset_avail_cnt].  Element 0 is fixed at
     FD_PACK_BITSET_SLOWPATH. */
  ushort bitset_avail[ 1UL+FD_PACK_BITSET_MAX ];
  ulong  bitset_avail_cnt;

  /* acct_to_bitset: an fd_map_dynamic that maps acct addresses to the
     reference count, which bit, etc. */
  fd_pack_bitset_acct_mapping_t * acct_to_bitset;

  /* chdkup: scratch memory chkdup needs for its internal processing */
  fd_chkdup_t chkdup[ 1 ];
};

typedef struct fd_pack_private fd_pack_t;

ulong
fd_pack_footprint( ulong                    pack_depth,
                   ulong                    bank_tile_cnt,
                   fd_pack_limits_t const * limits         ) {
  if( FD_UNLIKELY( (bank_tile_cnt==0) | (bank_tile_cnt>FD_PACK_MAX_BANK_TILES) ) ) return 0UL;
  if( FD_UNLIKELY( pack_depth<4UL ) ) return 0UL;

  ulong l;
  ulong max_acct_in_treap  = pack_depth * FD_TXN_ACCT_ADDR_MAX;
  ulong max_acct_in_flight = bank_tile_cnt * (FD_TXN_ACCT_ADDR_MAX * limits->max_txn_per_microblock + 1UL);

  ulong max_w_per_block    = fd_ulong_min( limits->max_cost_per_block / FD_PACK_COST_PER_WRITABLE_ACCT,
                                           limits->max_txn_per_microblock * limits->max_microblocks_per_block * FD_TXN_ACCT_ADDR_MAX );
  ulong written_list_max   = fd_ulong_min( max_w_per_block>>1, DEFAULT_WRITTEN_LIST_MAX );

  /* log base 2, but with a 2* so that the hash table stays sparse */
  int lg_uses_tbl_sz = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_flight ) );
  int lg_max_writers = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_w_per_block    ) );
  int lg_depth       = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*pack_depth         ) );
  int lg_acct_in_trp = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_treap  ) );

  l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, FD_PACK_ALIGN,      sizeof(fd_pack_t)                               );
  l = FD_LAYOUT_APPEND( l, trp_pool_align (),  trp_pool_footprint ( pack_depth+1UL           ) ); /* pool           */
  l = FD_LAYOUT_APPEND( l, expq_align     (),  expq_footprint     ( pack_depth+1UL           ) ); /* expiration prq */
  l = FD_LAYOUT_APPEND( l, acct_uses_align(),  acct_uses_footprint( lg_uses_tbl_sz           ) ); /* acct_in_use    */
  l = FD_LAYOUT_APPEND( l, acct_uses_align(),  acct_uses_footprint( lg_max_writers           ) ); /* writer_costs   */
  l = FD_LAYOUT_APPEND( l, 32UL,               sizeof(fd_pack_addr_use_t*)*written_list_max    ); /* written_list   */
  l = FD_LAYOUT_APPEND( l, sig2txn_align  (),  sig2txn_footprint  ( lg_depth                 ) ); /* signature_map  */
  l = FD_LAYOUT_APPEND( l, 32UL,               sizeof(fd_pack_addr_use_t)*max_acct_in_flight   ); /* use_by_bank    */
  l = FD_LAYOUT_APPEND( l, bitset_map_align(), bitset_map_footprint( lg_acct_in_trp          ) ); /* acct_to_bitset */
  return FD_LAYOUT_FINI( l, FD_PACK_ALIGN );
}

void *
fd_pack_new( void                   * mem,
             ulong                    pack_depth,
             ulong                    bank_tile_cnt,
             fd_pack_limits_t const * limits,
             fd_rng_t                * rng           ) {

  ulong max_acct_in_treap  = pack_depth * FD_TXN_ACCT_ADDR_MAX;
  ulong max_acct_in_flight = bank_tile_cnt * (FD_TXN_ACCT_ADDR_MAX * limits->max_txn_per_microblock + 1UL);
  ulong max_w_per_block    = fd_ulong_min( limits->max_cost_per_block / FD_PACK_COST_PER_WRITABLE_ACCT,
                                           limits->max_txn_per_microblock * limits->max_microblocks_per_block * FD_TXN_ACCT_ADDR_MAX );
  ulong written_list_max   = fd_ulong_min( max_w_per_block>>1, DEFAULT_WRITTEN_LIST_MAX );

  /* log base 2, but with a 2* so that the hash table stays sparse */
  int lg_uses_tbl_sz = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_flight ) );
  int lg_max_writers = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_w_per_block    ) );
  int lg_depth       = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*pack_depth         ) );
  int lg_acct_in_trp = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_treap  ) );

  FD_SCRATCH_ALLOC_INIT( l, mem );
  fd_pack_t * pack    = FD_SCRATCH_ALLOC_APPEND( l,  FD_PACK_ALIGN,       sizeof(fd_pack_t)                             );
  /* The pool has one extra element that is used between insert_init and
     cancel/fini. */
  void * _pool        = FD_SCRATCH_ALLOC_APPEND( l,  trp_pool_align(),    trp_pool_footprint ( pack_depth+1UL         ) );
  void * _expq        = FD_SCRATCH_ALLOC_APPEND( l,  expq_align(),        expq_footprint     ( pack_depth+1UL         ) );
  void * _uses        = FD_SCRATCH_ALLOC_APPEND( l,  acct_uses_align(),   acct_uses_footprint( lg_uses_tbl_sz         ) );
  void * _writer_cost = FD_SCRATCH_ALLOC_APPEND( l,  acct_uses_align(),   acct_uses_footprint( lg_max_writers         ) );
  void * _written_lst = FD_SCRATCH_ALLOC_APPEND( l,  32UL,                sizeof(fd_pack_addr_use_t*)*written_list_max  );
  void * _sig_map     = FD_SCRATCH_ALLOC_APPEND( l,  sig2txn_align(),     sig2txn_footprint  ( lg_depth               ) );
  void * _use_by_bank = FD_SCRATCH_ALLOC_APPEND( l,  32UL,                sizeof(fd_pack_addr_use_t)*max_acct_in_flight );
  void * _acct_bitset = FD_SCRATCH_ALLOC_APPEND( l,  bitset_map_align(),  bitset_map_footprint( lg_acct_in_trp        ) );

  pack->pack_depth                  = pack_depth;
  pack->bank_tile_cnt               = bank_tile_cnt;
  pack->lim[0]                      = *limits;
  pack->pending_txn_cnt             = 0UL;
  pack->microblock_cnt              = 0UL;
  pack->data_bytes_consumed         = 0UL;
  pack->rng                         = rng;
  pack->cumulative_block_cost       = 0UL;
  pack->cumulative_vote_cost        = 0UL;
  pack->expire_before               = 0UL;
  pack->outstanding_microblock_mask = 0UL;
  pack->cumulative_rebated_cus      = 0UL;


  trp_pool_new(  _pool,        pack_depth+1UL );

  fd_pack_ord_txn_t * pool = trp_pool_join( _pool );
  treap_seed( pool, pack_depth+1UL, fd_rng_ulong( rng ) );
  (void)trp_pool_leave( pool );


  treap_new( (void*)pack->pending,         pack_depth );
  treap_new( (void*)pack->pending_votes,   pack_depth );

  expq_new( _expq, pack_depth+1UL );

  FD_PACK_BITSET_CLEAR( pack->bitset_rw_in_use );
  FD_PACK_BITSET_CLEAR( pack->bitset_w_in_use  );

  acct_uses_new( _uses,        lg_uses_tbl_sz );
  acct_uses_new( _writer_cost, lg_max_writers );

  pack->written_list     = _written_lst;
  pack->written_list_cnt = 0UL;
  pack->written_list_max = written_list_max;

  sig2txn_new(   _sig_map,     lg_depth       );

  fd_pack_addr_use_t * use_by_bank = (fd_pack_addr_use_t *)_use_by_bank;
  for( ulong i=0UL; i<bank_tile_cnt; i++ ) pack->use_by_bank[i]=use_by_bank + i*(FD_TXN_ACCT_ADDR_MAX*limits->max_txn_per_microblock+1UL);
  for( ulong i=0UL; i<bank_tile_cnt; i++ ) pack->use_by_bank_cnt[i]=0UL;

  fd_histf_new( pack->txn_per_microblock,  FD_MHIST_MIN( PACK, TOTAL_TRANSACTIONS_PER_MICROBLOCK_COUNT ),
                                           FD_MHIST_MAX( PACK, TOTAL_TRANSACTIONS_PER_MICROBLOCK_COUNT ) );
  fd_histf_new( pack->vote_per_microblock, FD_MHIST_MIN( PACK, VOTES_PER_MICROBLOCK_COUNT ),
                                           FD_MHIST_MAX( PACK, VOTES_PER_MICROBLOCK_COUNT ) );

  fd_histf_new( pack->scheduled_cus_per_block, FD_MHIST_MIN( PACK, CUS_SCHEDULED ),
                                               FD_MHIST_MAX( PACK, CUS_SCHEDULED ) );
  fd_histf_new( pack->rebated_cus_per_block,   FD_MHIST_MIN( PACK, CUS_REBATED   ),
                                               FD_MHIST_MAX( PACK, CUS_REBATED   ) );
  fd_histf_new( pack->net_cus_per_block,       FD_MHIST_MIN( PACK, CUS_NET       ),
                                               FD_MHIST_MAX( PACK, CUS_NET       ) );


  pack->bitset_avail[ 0 ] = FD_PACK_BITSET_SLOWPATH;
  for( ulong i=0UL; i<FD_PACK_BITSET_MAX; i++ ) pack->bitset_avail[ i+1UL ] = (ushort)i;
  pack->bitset_avail_cnt = FD_PACK_BITSET_MAX;

  bitset_map_new( _acct_bitset, lg_acct_in_trp );

  fd_chkdup_new( pack->chkdup, rng );

  return mem;
}

fd_pack_t *
fd_pack_join( void * mem ) {
  FD_SCRATCH_ALLOC_INIT( l, mem );
  fd_pack_t * pack  = FD_SCRATCH_ALLOC_APPEND( l, FD_PACK_ALIGN, sizeof(fd_pack_t) );

  ulong pack_depth             = pack->pack_depth;
  ulong bank_tile_cnt          = pack->bank_tile_cnt;

  ulong max_acct_in_treap  = pack_depth * FD_TXN_ACCT_ADDR_MAX;
  ulong max_acct_in_flight = bank_tile_cnt * (FD_TXN_ACCT_ADDR_MAX * pack->lim->max_txn_per_microblock + 1UL);
  ulong max_w_per_block    = fd_ulong_min( pack->lim->max_cost_per_block / FD_PACK_COST_PER_WRITABLE_ACCT,
                                           pack->lim->max_txn_per_microblock * pack->lim->max_microblocks_per_block * FD_TXN_ACCT_ADDR_MAX );
  ulong written_list_max   = fd_ulong_min( max_w_per_block>>1, DEFAULT_WRITTEN_LIST_MAX );

  int lg_uses_tbl_sz = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_flight ) );
  int lg_max_writers = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_w_per_block    ) );
  int lg_depth       = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*pack_depth         ) );
  int lg_acct_in_trp = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_treap  ) );


  pack->pool          = trp_pool_join(   FD_SCRATCH_ALLOC_APPEND( l, trp_pool_align(),   trp_pool_footprint ( pack_depth+1UL ) ) );
  pack->expiration_q  = expq_join    (   FD_SCRATCH_ALLOC_APPEND( l, expq_align(),       expq_footprint     ( pack_depth+1UL ) ) );
  pack->acct_in_use   = acct_uses_join(  FD_SCRATCH_ALLOC_APPEND( l, acct_uses_align(),  acct_uses_footprint( lg_uses_tbl_sz ) ) );
  pack->writer_costs  = acct_uses_join(  FD_SCRATCH_ALLOC_APPEND( l, acct_uses_align(),  acct_uses_footprint( lg_max_writers ) ) );
  /* */                                  FD_SCRATCH_ALLOC_APPEND( l, 32UL,               sizeof(fd_pack_addr_use_t*)*written_list_max  );
  pack->signature_map = sig2txn_join(    FD_SCRATCH_ALLOC_APPEND( l, sig2txn_align(),    sig2txn_footprint  ( lg_depth       ) ) );
  /* */                                  FD_SCRATCH_ALLOC_APPEND( l, 32UL,               sizeof(fd_pack_addr_use_t)*max_acct_in_flight );
  pack->acct_to_bitset= bitset_map_join( FD_SCRATCH_ALLOC_APPEND( l, bitset_map_align(), bitset_map_footprint( lg_acct_in_trp) ) );

  FD_MGAUGE_SET( PACK, PENDING_TRANSACTIONS_HEAP_SIZE, pack_depth );
  return pack;
}



static int
fd_pack_estimate_rewards_and_compute( fd_txn_p_t        * txnp,
                                      fd_pack_ord_txn_t * out ) {
  fd_txn_t * txn = TXN(txnp);
  ulong sig_rewards = FD_PACK_FEE_PER_SIGNATURE * txn->signature_cnt; /* Easily in [5000, 635000] */

  ulong execution_cus;
  ulong adtl_rewards;
  ulong precompile_sigs;
  ulong cost = fd_pack_compute_cost( txnp, &txnp->flags, &execution_cus, &adtl_rewards, &precompile_sigs );

  if( FD_UNLIKELY( !cost ) ) return 0;

  /* precompile_sigs <= 16320, so after the addition,
     sig_rewards < 83,000,000 */
  sig_rewards += FD_PACK_FEE_PER_SIGNATURE * precompile_sigs;

  /* No fancy CU estimation in this version of pack
  for( ulong i=0UL; i<(ulong)txn->instr_cnt; i++ ) {
    uchar prog_id_idx = txn->instr[ i ].program_id;
    fd_acct_addr_t const * acct_addr = fd_txn_get_acct_addrs( txn, txnp->payload ) + (ulong)prog_id_idx;
  }
  */
  out->rewards            = (adtl_rewards < (UINT_MAX - sig_rewards)) ? (uint)(sig_rewards + adtl_rewards) : UINT_MAX;
  out->compute_est        = (uint)cost;
  out->txn->requested_cus = (uint)execution_cus;

  out->root = (txnp->flags & FD_TXN_P_FLAGS_IS_SIMPLE_VOTE) ? FD_ORD_TXN_ROOT_PENDING_VOTE : FD_ORD_TXN_ROOT_PENDING;

#if DETAILED_LOGGING
  FD_LOG_NOTICE(( "TXN estimated compute %lu+-%f. Rewards: %lu + %lu", compute_expected, (double)compute_variance, sig_rewards, adtl_rewards ));
#endif

  return 1;
}

/* Can the fee payer afford to pay a transaction with the specified
   price?  Returns 1 if so, 0 otherwise.  This is just a stub that
   always returns 1 for now.  In general, this function can't be totally
   accurate, because the transactions immediately prior to this one can
   affect the balance of this fee payer, but a simple check here may be
   helpful for reducing spam. */
static int
fd_pack_can_fee_payer_afford( fd_acct_addr_t const * acct_addr,
                              ulong                  price /* in lamports */) {
  (void)acct_addr;
  (void)price;
  return 1;
}





fd_txn_p_t * fd_pack_insert_txn_init(   fd_pack_t * pack                   ) { return trp_pool_ele_acquire( pack->pool )->txn; }
void         fd_pack_insert_txn_cancel( fd_pack_t * pack, fd_txn_p_t * txn ) { trp_pool_ele_release( pack->pool, (fd_pack_ord_txn_t*)txn ); }

#define REJECT( reason ) do {                                       \
                           trp_pool_ele_release( pack->pool, ord ); \
                           return FD_PACK_INSERT_REJECT_ ## reason; \
                         } while( 0 )

int
fd_pack_insert_txn_fini( fd_pack_t  * pack,
                         fd_txn_p_t * txnp,
                         ulong        expires_at ) {

  fd_pack_ord_txn_t * ord = (fd_pack_ord_txn_t *)txnp;

  fd_txn_t * txn   = TXN(txnp);
  uchar * payload  = txnp->payload;

  fd_acct_addr_t const * accts = fd_txn_get_acct_addrs( txn, payload );
  ulong imm_cnt = fd_txn_account_cnt( txn, FD_TXN_ACCT_CAT_IMM );

  if( FD_UNLIKELY( !fd_pack_estimate_rewards_and_compute( txnp, ord ) ) ) REJECT( ESTIMATION_FAIL );

  ord->expires_at = expires_at;
  int is_vote = ord->root==FD_ORD_TXN_ROOT_PENDING_VOTE;

  int writes_to_sysvar = 0;
  for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
      iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
    writes_to_sysvar |= fd_pack_unwritable_contains( accts+fd_txn_acct_iter_idx( iter ) );
  }

  fd_ed25519_sig_t const * sig = fd_txn_get_signatures( txn, payload );
  fd_chkdup_t * chkdup = pack->chkdup;

  /* Throw out transactions ... */
  /*           ... that are unfunded */
  if( FD_UNLIKELY( !fd_pack_can_fee_payer_afford( accts, ord->rewards ) ) ) REJECT( UNAFFORDABLE  );
  /*           ... that are so big they'll never run */
  if( FD_UNLIKELY( ord->compute_est >= pack->lim->max_cost_per_block    ) ) REJECT( TOO_LARGE     );
  /*           ... that load too many accounts (ignoring 9LZdXeKGeBV6hRLdxS1rHbHoEUsKqesCC2ZAPTPKJAbK) */
  if( FD_UNLIKELY( fd_txn_account_cnt( txn, FD_TXN_ACCT_CAT_ALL )>64UL  ) ) REJECT( ACCOUNT_CNT   );
  /*           ... that duplicate an account address */
  if( FD_UNLIKELY( fd_chkdup_check( chkdup, accts, imm_cnt, NULL, 0UL ) ) ) REJECT( DUPLICATE_ACCT );
  /*           ... that try to write to a sysvar */
  if( FD_UNLIKELY( writes_to_sysvar                                     ) ) REJECT( WRITES_SYSVAR );
  /*           ... that we already know about */
  if( FD_UNLIKELY( sig2txn_query( pack->signature_map, sig, NULL )      ) ) REJECT( DUPLICATE     );
  /*           ... that have already expired */
  if( FD_UNLIKELY( expires_at<pack->expire_before                       ) ) REJECT( EXPIRED       );
  /*           ... that additional accounts from an ALT */
  if( FD_UNLIKELY( txn->addr_table_adtl_cnt>0UL                         ) ) REJECT( ADDR_LUT      );


  int replaces = 0;
  if( FD_UNLIKELY( pack->pending_txn_cnt == pack->pack_depth ) ) {
    /* If the tree is full, we want to see if this is better than the
       worst element in the treap before inserting.  If the new
       transaction is better than that one, we'll delete it and insert
       the new transaction. Otherwise, we'll throw away this
       transaction.  We want to make sure we provide reasonable quality
       of service for votes based on what fraction of the treap is votes
       though, so we'll employ the following policy:

         Case             New Vote                 New Non-vote
       Votes < 25%   Replace worst non-vote    If better, replace worst
                     with it                   non-vote with it

       Votes > 75%   If better, replace        Replace worst vote with
                     worst vote with it        it

       Else          If better, replace worse of worst non-vote and
                     worst vote                                        */
    ulong vote_cnt     = treap_ele_cnt( pack->pending_votes ); int low_votes    = (vote_cnt    <(pack->pack_depth>>2));
    ulong non_vote_cnt = treap_ele_cnt( pack->pending       ); int low_nonvotes = (non_vote_cnt<(pack->pack_depth>>2));
    int pool_imbalanced = low_votes | low_nonvotes;
    int improves_balance = (low_votes & is_vote) | (low_nonvotes & !is_vote);

    /* Need to check that corresponding treap was not empty before dereferencing
       these two pointers. */
    fd_pack_ord_txn_t * worst_vote    = treap_fwd_iter_ele( treap_fwd_iter_init( pack->pending_votes, pack->pool ), pack->pool );
    fd_pack_ord_txn_t * worst_nonvote = treap_fwd_iter_ele( treap_fwd_iter_init( pack->pending,       pack->pool ), pack->pool );
    fd_pack_ord_txn_t * worst = NULL;

    /* In the imbalanced case, there are two symmetric cases.
       Considering just the first one, low_nonvotes==true implies
             vote_cnt > pack_depth - (pack_depth>>2)
       Which means vote_cnt>0.  In the imbalanced case when
       low_nonvotes==false, we know low_votes must be true, so similar
       logic applies.
       In the balanced case, vote_cnt and non_vote_cnt are both at least
       as large as (pack_depth>>2) >= 1, since pack_depth>=4 so
       worst_vote and worst_nonvote are safe, implying worst is safe. */
    if( pool_imbalanced ) worst = fd_ptr_if( low_nonvotes,                               worst_vote, worst_nonvote );
    else                  worst = fd_ptr_if( COMPARE_WORSE( worst_vote, worst_nonvote ), worst_vote, worst_nonvote );

    if( FD_LIKELY( improves_balance || COMPARE_WORSE( worst, ord ) ) ) {
      replaces = 1;
      fd_ed25519_sig_t const * worst_sig = fd_txn_get_signatures( TXN( worst->txn ), worst->txn->payload );
      fd_pack_delete_transaction( pack, worst_sig );
    } else {
      REJECT( PRIORITY );
    }
  }

  /* At this point, we know we have space to insert the transaction and
     we've committed to insert it. */

  FD_PACK_BITSET_CLEAR( ord->rw_bitset );
  FD_PACK_BITSET_CLEAR( ord->w_bitset  );

  for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
      iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
    fd_acct_addr_t acct = accts[fd_txn_acct_iter_idx( iter )];
    fd_pack_bitset_acct_mapping_t * q = bitset_map_query( pack->acct_to_bitset, acct, NULL );
    if( FD_UNLIKELY( q==NULL ) ) {
      q = bitset_map_insert( pack->acct_to_bitset, acct );
      q->ref_cnt                  = 0UL;
      q->first_instance           = ord;
      q->first_instance_was_write = 1;
      q->bit                      = FD_PACK_BITSET_FIRST_INSTANCE;
    } else if( FD_UNLIKELY( q->bit == FD_PACK_BITSET_FIRST_INSTANCE ) ) {
      q->bit = pack->bitset_avail[ pack->bitset_avail_cnt ];
      pack->bitset_avail_cnt = fd_ulong_if( !!pack->bitset_avail_cnt, pack->bitset_avail_cnt-1UL, 0UL );

      FD_PACK_BITSET_SETN( q->first_instance->rw_bitset, q->bit );
      if( q->first_instance_was_write ) FD_PACK_BITSET_SETN( q->first_instance->w_bitset, q->bit );
    }

    q->ref_cnt++;
    FD_PACK_BITSET_SETN( ord->rw_bitset, q->bit );
    FD_PACK_BITSET_SETN( ord->w_bitset , q->bit );
  }

  for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_READONLY & FD_TXN_ACCT_CAT_IMM );
      iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

    fd_acct_addr_t acct = accts[fd_txn_acct_iter_idx( iter )];
    if( FD_UNLIKELY( fd_pack_unwritable_contains( &acct ) ) ) continue;

    fd_pack_bitset_acct_mapping_t * q = bitset_map_query( pack->acct_to_bitset, acct, NULL );
    if( FD_UNLIKELY( q==NULL ) ) {
      q = bitset_map_insert( pack->acct_to_bitset, acct );
      q->ref_cnt                  = 0UL;
      q->first_instance           = ord;
      q->first_instance_was_write = 0;
      q->bit                      = FD_PACK_BITSET_FIRST_INSTANCE;
    } else if( FD_UNLIKELY( q->bit == FD_PACK_BITSET_FIRST_INSTANCE ) ) {
      q->bit = pack->bitset_avail[ pack->bitset_avail_cnt ];
      pack->bitset_avail_cnt = fd_ulong_if( !!pack->bitset_avail_cnt, pack->bitset_avail_cnt-1UL, 0UL );

      FD_PACK_BITSET_SETN( q->first_instance->rw_bitset, q->bit );
      if( q->first_instance_was_write ) FD_PACK_BITSET_SETN( q->first_instance->w_bitset, q->bit );
    }

    q->ref_cnt++;
    FD_PACK_BITSET_SETN( ord->rw_bitset, q->bit );
  }

  pack->pending_txn_cnt++;

  sig2txn_insert( pack->signature_map, fd_txn_get_signatures( txn, payload ) );

  fd_pack_expq_t temp[ 1 ] = {{ .expires_at = expires_at, .txn = ord }};
  expq_insert( pack->expiration_q, temp );

  if( FD_LIKELY( ord->root == FD_ORD_TXN_ROOT_PENDING_VOTE ) ) {
    treap_ele_insert( pack->pending_votes, ord, pack->pool );
    return replaces ? FD_PACK_INSERT_ACCEPT_VOTE_REPLACE : FD_PACK_INSERT_ACCEPT_VOTE_ADD;
  } else {
    treap_ele_insert( pack->pending,       ord, pack->pool );
    return replaces ? FD_PACK_INSERT_ACCEPT_NONVOTE_REPLACE : FD_PACK_INSERT_ACCEPT_NONVOTE_ADD;
  }
}
#undef REJECT

typedef struct {
  ushort clear_rw_bit;
  ushort clear_w_bit;
} release_result_t;

static inline release_result_t
release_bit_reference( fd_pack_t            * pack,
                       fd_acct_addr_t const * acct ) {

  fd_pack_bitset_acct_mapping_t * q = bitset_map_query( pack->acct_to_bitset, *acct, NULL );
  FD_TEST( q ); /* q==NULL not be possible */

  q->ref_cnt--;

  if( FD_UNLIKELY( q->ref_cnt==0UL ) ) {
    ushort bit = q->bit;
    bitset_map_remove( pack->acct_to_bitset, q );
    if( FD_LIKELY( bit<FD_PACK_BITSET_MAX ) ) pack->bitset_avail[ ++(pack->bitset_avail_cnt) ] = bit;

    fd_pack_addr_use_t * use = acct_uses_query( pack->acct_in_use,  *acct, NULL );
    if( FD_LIKELY( use ) ) {
      use->in_use_by |= FD_PACK_IN_USE_BIT_CLEARED;
      release_result_t ret = { .clear_rw_bit = bit,
                               .clear_w_bit = fd_ushort_if( !!(use->in_use_by & FD_PACK_IN_USE_WRITABLE), bit, FD_PACK_BITSET_MAX ) };
      return ret;
    }
  }
  release_result_t ret = { .clear_rw_bit = FD_PACK_BITSET_MAX, .clear_w_bit = FD_PACK_BITSET_MAX };
  return ret;
}

typedef struct {
  ulong cus_scheduled;
  ulong txns_scheduled;
  ulong bytes_scheduled;
} sched_return_t;

static inline sched_return_t
fd_pack_schedule_impl( fd_pack_t  * pack,
                       treap_t    * sched_from,
                       ulong        cu_limit,
                       ulong        txn_limit,
                       ulong        byte_limit,
                       ulong        bank_tile,
                       fd_txn_p_t * out ) {

  fd_pack_ord_txn_t  * pool         = pack->pool;
  fd_pack_addr_use_t * acct_in_use  = pack->acct_in_use;
  fd_pack_addr_use_t * writer_costs = pack->writer_costs;

  fd_pack_addr_use_t ** written_list     = pack->written_list;
  ulong                 written_list_cnt = pack->written_list_cnt;
  ulong                 written_list_max = pack->written_list_max;

  FD_PACK_BITSET_DECLARE( bitset_rw_in_use );
  FD_PACK_BITSET_DECLARE( bitset_w_in_use  );
  FD_PACK_BITSET_COPY( bitset_rw_in_use, pack->bitset_rw_in_use );
  FD_PACK_BITSET_COPY( bitset_w_in_use,  pack->bitset_w_in_use  );

  fd_pack_addr_use_t * use_by_bank     = pack->use_by_bank    [bank_tile];
  ulong                use_by_bank_cnt = pack->use_by_bank_cnt[bank_tile];

  ulong max_write_cost_per_acct = pack->lim->max_write_cost_per_acct;

  ulong txns_scheduled  = 0UL;
  ulong cus_scheduled   = 0UL;
  ulong bytes_scheduled = 0UL;

  ulong bank_tile_mask = 1UL << bank_tile;

  ulong fast_path     = 0UL;
  ulong slow_path     = 0UL;
  ulong cu_limit_c    = 0UL;
  ulong byte_limit_c  = 0UL;
  ulong write_limit_c = 0UL;

  if( FD_UNLIKELY( (cu_limit<FD_PACK_MIN_TXN_COST) | (txn_limit==0UL) | (byte_limit<FD_TXN_MIN_SERIALIZED_SZ) ) ) {
    sched_return_t to_return = { .cus_scheduled = 0UL, .txns_scheduled = 0UL, .bytes_scheduled = 0UL };
    return to_return;
  }

  treap_rev_iter_t prev;
  for( treap_rev_iter_t _cur=treap_rev_iter_init( sched_from, pool ); !treap_rev_iter_done( _cur ); _cur=prev ) {
    /* Capture next so that we can delete while we iterate. */
    prev = treap_rev_iter_next( _cur, pool );

#   if FD_HAS_X86
    _mm_prefetch( &(pool[ prev ].prev),      _MM_HINT_T0 );
#   endif

    fd_pack_ord_txn_t const * cur = treap_rev_iter_ele_const( _cur, pool );

    ulong conflicts = 0UL;

    if( FD_UNLIKELY( cur->compute_est>cu_limit ) ) {
      /* Too big to be scheduled at the moment, but might be okay for
         the next microblock, so we don't want to delay it. */
      cu_limit_c++;
      continue;
    }

    /* Likely? Unlikely? */
    if( FD_LIKELY( !FD_PACK_BITSET_INTERSECT4_EMPTY( bitset_rw_in_use, bitset_w_in_use, cur->w_bitset, cur->rw_bitset ) ) ) {
      fast_path++;
      continue;
    }

    if( FD_UNLIKELY( cur->txn->payload_sz>byte_limit ) ) {
      byte_limit_c++;
      continue;
    }

    fd_txn_t const * txn = TXN(cur->txn);
    fd_acct_addr_t const * acct = fd_txn_get_acct_addrs( txn, cur->txn->payload );
    /* Check conflicts between this transaction's writable accounts and
       current readers */
    for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
        iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

      ulong i=fd_txn_acct_iter_idx( iter );

      fd_pack_addr_use_t * in_wcost_table = acct_uses_query( writer_costs, acct[i], NULL );
      if( FD_UNLIKELY( in_wcost_table && in_wcost_table->total_cost+cur->compute_est > max_write_cost_per_acct ) ) {
        /* Can't be scheduled until the next block */
        conflicts = ULONG_MAX;
        break;
      }

      fd_pack_addr_use_t * use = acct_uses_query( acct_in_use, acct[i], NULL );
      if( FD_UNLIKELY( use ) ) conflicts |= use->in_use_by; /* break? */
    }

    if( FD_UNLIKELY( conflicts==ULONG_MAX ) ) {
      write_limit_c++;
      continue;
    }

    if( FD_UNLIKELY( conflicts ) ) {
      slow_path++;
      continue;
    }

    /* Check conflicts between this transaction's readonly accounts and
       current writers */
    for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_READONLY & FD_TXN_ACCT_CAT_IMM );
        iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

      ulong i=fd_txn_acct_iter_idx( iter );
      if( fd_pack_unwritable_contains( acct+i ) ) continue; /* No need to track sysvars because they can't be writable */

      fd_pack_addr_use_t * use = acct_uses_query( acct_in_use,  acct[i], NULL );
      if( use ) conflicts |= (use->in_use_by & FD_PACK_IN_USE_WRITABLE) ? use->in_use_by : 0UL;
    }

    if( FD_UNLIKELY( conflicts ) ) {
      slow_path++;
      continue;
    }

    /* Include this transaction in the microblock! */
    FD_PACK_BITSET_OR( bitset_rw_in_use, cur->rw_bitset );
    FD_PACK_BITSET_OR( bitset_w_in_use,  cur->w_bitset  );

    fd_memcpy( out->payload, cur->txn->payload, cur->txn->payload_sz                                           );
    fd_memcpy( TXN(out),     txn,               fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt ) );
    out->payload_sz    = cur->txn->payload_sz;
    out->requested_cus = cur->txn->requested_cus;
    out->executed_cus  = 0U;
    out->flags         = cur->txn->flags;
    out++;

    for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
        iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
      fd_acct_addr_t acct_addr = acct[fd_txn_acct_iter_idx( iter )];

      fd_pack_addr_use_t * in_wcost_table = acct_uses_query( writer_costs, acct_addr, NULL );
      if( !in_wcost_table ) {
        in_wcost_table = acct_uses_insert( writer_costs, acct_addr );
        in_wcost_table->total_cost = 0UL;
        written_list[ written_list_cnt ] = in_wcost_table;
        written_list_cnt = fd_ulong_min( written_list_cnt+1UL, written_list_max-1UL );
      }
      in_wcost_table->total_cost += cur->compute_est;

      fd_pack_addr_use_t * use = acct_uses_insert( acct_in_use, acct_addr );
      use->in_use_by = bank_tile_mask | FD_PACK_IN_USE_WRITABLE;

      use_by_bank[use_by_bank_cnt++] = *use;

      /* If there aren't any more references to this account in the
         heap, it can't cause any conflicts.  That means we actually
         don't need to record that we are using it, which is good
         because we want to release the bit. */
      release_result_t ret = release_bit_reference( pack, &acct_addr );
      FD_PACK_BITSET_CLEARN( bitset_rw_in_use, ret.clear_rw_bit );
      FD_PACK_BITSET_CLEARN( bitset_w_in_use,  ret.clear_w_bit  );
    }
    for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_READONLY & FD_TXN_ACCT_CAT_IMM );
        iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

      fd_acct_addr_t acct_addr = acct[fd_txn_acct_iter_idx( iter )];

      if( fd_pack_unwritable_contains( &acct_addr ) ) continue; /* No need to track sysvars because they can't be writable */

      fd_pack_addr_use_t * use = acct_uses_query( acct_in_use,  acct_addr, NULL );
      if( !use ) { use = acct_uses_insert( acct_in_use, acct_addr ); use->in_use_by = 0UL; }

      if( !(use->in_use_by & bank_tile_mask) ) use_by_bank[use_by_bank_cnt++] = *use;
      use->in_use_by |= bank_tile_mask;
      use->in_use_by &= ~FD_PACK_IN_USE_BIT_CLEARED;


      release_result_t ret = release_bit_reference( pack, &acct_addr );
      FD_PACK_BITSET_CLEARN( bitset_rw_in_use, ret.clear_rw_bit );
      FD_PACK_BITSET_CLEARN( bitset_w_in_use,  ret.clear_w_bit  );
    }

    txns_scheduled  += 1UL;                      txn_limit       -= 1UL;
    cus_scheduled   += cur->compute_est;         cu_limit        -= cur->compute_est;
    bytes_scheduled += cur->txn->payload_sz;     byte_limit      -= cur->txn->payload_sz;

    fd_ed25519_sig_t const * sig0 = fd_txn_get_signatures( txn, cur->txn->payload );

    fd_pack_sig_to_txn_t * in_tbl = sig2txn_query( pack->signature_map, sig0, NULL );
    sig2txn_remove( pack->signature_map, in_tbl );

    expq_remove( pack->expiration_q, cur->expq_idx );
    treap_idx_remove( sched_from, _cur, pool );
    trp_pool_idx_release( pool, _cur );
    pack->pending_txn_cnt--;

    if( FD_UNLIKELY( (cu_limit<FD_PACK_MIN_TXN_COST) | (txn_limit==0UL) | (byte_limit<FD_TXN_MIN_SERIALIZED_SZ) ) ) break;
  }

  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_TAKEN,      txns_scheduled );
  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_CU_LIMIT,   cu_limit_c     );
  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_FAST_PATH,  fast_path      );
  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_BYTE_LIMIT, byte_limit_c   );
  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_WRITE_COST, write_limit_c  );
  FD_MCNT_INC( PACK, TRANSACTION_SCHEDULE_SLOW_PATH,  slow_path      );

#if DETAILED_LOGGING
  FD_LOG_NOTICE(( "cu_limit: %lu, fast_path: %lu, slow_path: %lu", cu_limit_c, fast_path, slow_path ));
#endif

  pack->use_by_bank_cnt[bank_tile] = use_by_bank_cnt;
  FD_PACK_BITSET_COPY( pack->bitset_rw_in_use, bitset_rw_in_use );
  FD_PACK_BITSET_COPY( pack->bitset_w_in_use,  bitset_w_in_use  );

  pack->written_list_cnt = written_list_cnt;

  sched_return_t to_return = { .cus_scheduled=cus_scheduled, .txns_scheduled=txns_scheduled, .bytes_scheduled=bytes_scheduled };
  return to_return;
}

void
fd_pack_microblock_complete( fd_pack_t * pack,
                             ulong       bank_tile ) {
  /* If the account is in use writably, and it's in use by this banking
     tile, then this banking tile must be the sole writer to it, so it's
     always okay to clear the writable bit. */
  ulong clear_mask = ~((1UL<<bank_tile) | FD_PACK_IN_USE_WRITABLE);

  FD_PACK_BITSET_DECLARE( bitset_rw_in_use );
  FD_PACK_BITSET_DECLARE( bitset_w_in_use  );
  FD_PACK_BITSET_COPY( bitset_rw_in_use, pack->bitset_rw_in_use );
  FD_PACK_BITSET_COPY( bitset_w_in_use,  pack->bitset_w_in_use  );

  fd_pack_addr_use_t * base = pack->use_by_bank[bank_tile];
  for( ulong i=0UL; i<pack->use_by_bank_cnt[bank_tile]; i++ ) {
    fd_pack_addr_use_t * use = acct_uses_query( pack->acct_in_use, base[i].key, NULL );
    FD_TEST( use );
    use->in_use_by &= clear_mask;

    /* In order to properly bound the size of bitset_map, we need to
       release the "reference" to the account when we schedule it.
       However, that poses a bit of a problem here, because by the time
       we complete the microblock, that account could have been assigned
       a different bit in the bitset.  The scheduling step tells us if
       that is the case, and if so, we know that the bits in
       bitset_w_in_use and bitset_rw_in_use were already cleared as
       necessary.

       Note that it's possible for BIT_CLEARED to be set and then unset
       by later uses, but then the account would be in use on other
       banks, so we wouldn't try to observe the old value.  For example:
       Suppose bit 0->account A, bit 1->account B, and we have two
       transactions that read A, B.  We schedule a microblock to bank 0,
       taking both transactions, which sets the counts for A, B to 0,
       and releases the bits, clearing bits 0 and 1, and setting
       BIT_CLEARED.  Then we get two more transactions that read
       accounts C, D, A, B, and they get assigned 0->C, 1->D, 2->A,
       3->B.  We try to schedule a microblock to bank 1 that takes one
       of those transactions.   This unsets BIT_CLEARED for A, B.
       Finally, the first microblock completes.  Even though the bitset
       map has the new bits for A and B which are "wrong" compared to
       when the transaction was initially scheduled, those bits have
       already been cleared and reset properly in the bitset as needed.
       A and B will still be in use by bank 1, so we won't clear any
       bits.  If, on the other hand, the microblock scheduled to bank 1
       completes first, bits 0 and 1 will be cleared for accounts C and
       D, while bits 2 and 3 will remain set, which is correct.  Then
       when bank 0 completes, bits 2 and 3 will be cleared. */
    if( FD_LIKELY( !use->in_use_by ) ) { /* if in_use_by==0, doesn't include BIT_CLEARED */
      fd_pack_bitset_acct_mapping_t * q = bitset_map_query( pack->acct_to_bitset, base[i].key, NULL );
      FD_TEST( q );
      FD_PACK_BITSET_CLEARN( bitset_w_in_use,  q->bit );
      FD_PACK_BITSET_CLEARN( bitset_rw_in_use, q->bit );
    }
    if( FD_LIKELY( !(use->in_use_by & ~FD_PACK_IN_USE_BIT_CLEARED) ) ) acct_uses_remove( pack->acct_in_use, use );
  }

  pack->use_by_bank_cnt[bank_tile] = 0UL;

  FD_PACK_BITSET_COPY( pack->bitset_rw_in_use, bitset_rw_in_use );
  FD_PACK_BITSET_COPY( pack->bitset_w_in_use,  bitset_w_in_use  );

  /* outstanding_microblock_mask never has the writable bit set, so we
     don't care about clearing it here either. */
  pack->outstanding_microblock_mask &= clear_mask;
}


ulong
fd_pack_schedule_next_microblock( fd_pack_t *  pack,
                                  ulong        total_cus,
                                  float        vote_fraction,
                                  ulong        bank_tile,
                                  fd_txn_p_t * out ) {

  /* TODO: Decide if these are exactly how we want to handle limits */
  total_cus = fd_ulong_min( total_cus, pack->lim->max_cost_per_block - pack->cumulative_block_cost );
  ulong vote_cus = fd_ulong_min( (ulong)((float)total_cus * vote_fraction),
                                 pack->lim->max_vote_cost_per_block - pack->cumulative_vote_cost );
  ulong vote_reserved_txns = fd_ulong_min( vote_cus/FD_PACK_TYPICAL_VOTE_COST,
                                           (ulong)((float)pack->lim->max_txn_per_microblock * vote_fraction) );


  if( FD_UNLIKELY( (pack->microblock_cnt>=pack->lim->max_microblocks_per_block) ) ) {
    FD_MCNT_INC( PACK, MICROBLOCK_PER_BLOCK_LIMIT, 1UL );
    return 0UL;
  }
  if( FD_UNLIKELY( pack->data_bytes_consumed+MICROBLOCK_DATA_OVERHEAD+FD_TXN_MIN_SERIALIZED_SZ>pack->lim->max_data_bytes_per_block) ) {
    FD_MCNT_INC( PACK, DATA_PER_BLOCK_LIMIT, 1UL );
    return 0UL;
  }

  ulong cu_limit  = total_cus - vote_cus;
  ulong txn_limit = pack->lim->max_txn_per_microblock - vote_reserved_txns;
  ulong scheduled = 0UL;
  ulong byte_limit = pack->lim->max_data_bytes_per_block - pack->data_bytes_consumed - MICROBLOCK_DATA_OVERHEAD;

  sched_return_t status, status1;

  /* Try to schedule non-vote transactions */
  status = fd_pack_schedule_impl( pack, pack->pending,       cu_limit, txn_limit,          byte_limit, bank_tile, out+scheduled );

  scheduled                   += status.txns_scheduled;            txn_limit  -= status.txns_scheduled;
  pack->cumulative_block_cost += status.cus_scheduled;             cu_limit   -= status.cus_scheduled;
  pack->data_bytes_consumed   += status.bytes_scheduled;           byte_limit -= status.bytes_scheduled;


  /* Schedule vote transactions */
  status1= fd_pack_schedule_impl( pack, pack->pending_votes, vote_cus, vote_reserved_txns, byte_limit, bank_tile, out+scheduled );

  scheduled                   += status1.txns_scheduled;
  pack->cumulative_vote_cost  += status1.cus_scheduled;
  pack->cumulative_block_cost += status1.cus_scheduled;
  pack->data_bytes_consumed   += status1.bytes_scheduled;
  byte_limit                  -= status1.bytes_scheduled;
  /* Add any remaining CUs/txns to the non-vote limits */
  txn_limit += vote_reserved_txns - status1.txns_scheduled;
  cu_limit  += vote_cus - status1.cus_scheduled;


  /* Fill any remaining space with non-vote transactions */
  status = fd_pack_schedule_impl( pack, pack->pending,       cu_limit, txn_limit,          byte_limit, bank_tile, out+scheduled );

  scheduled                   += status.txns_scheduled;
  pack->cumulative_block_cost += status.cus_scheduled;
  pack->data_bytes_consumed   += status.bytes_scheduled;

  ulong nonempty = (ulong)(scheduled>0UL);
  pack->microblock_cnt              += nonempty;
  pack->outstanding_microblock_mask |= nonempty << bank_tile;
  pack->data_bytes_consumed         += nonempty * MICROBLOCK_DATA_OVERHEAD;

  /* Update metrics counters */
  FD_MGAUGE_SET( PACK, AVAILABLE_TRANSACTIONS,      pack->pending_txn_cnt                );
  FD_MGAUGE_SET( PACK, AVAILABLE_VOTE_TRANSACTIONS, treap_ele_cnt( pack->pending_votes ) );
  FD_MGAUGE_SET( PACK, CUS_CONSUMED_IN_BLOCK,       pack->cumulative_block_cost          );

  fd_histf_sample( pack->txn_per_microblock,  scheduled              );
  fd_histf_sample( pack->vote_per_microblock, status1.txns_scheduled );

  return scheduled;
}

ulong fd_pack_avail_txn_cnt( fd_pack_t const * pack ) { return pack->pending_txn_cnt; }
ulong fd_pack_bank_tile_cnt( fd_pack_t const * pack ) { return pack->bank_tile_cnt;   }


void
fd_pack_set_block_limits( fd_pack_t * pack,
                          ulong       max_microblocks_per_block,
                          ulong       max_data_bytes_per_block ) {
  pack->lim->max_microblocks_per_block = max_microblocks_per_block;
  pack->lim->max_data_bytes_per_block  = max_data_bytes_per_block;
}

void
fd_pack_rebate_cus( fd_pack_t        * pack,
                    fd_txn_p_t const * txns,
                    ulong              txn_cnt ) {
  fd_pack_addr_use_t * writer_costs = pack->writer_costs;

  ulong cumulative_vote_cost   = pack->cumulative_vote_cost;
  ulong cumulative_block_cost  = pack->cumulative_block_cost;
  ulong data_bytes_consumed    = pack->data_bytes_consumed;
  ulong cumulative_rebated_cus = pack->cumulative_rebated_cus;

  for( ulong i=0UL; i<txn_cnt; i++ ) {
    fd_txn_p_t const * txn = txns+i;
    ulong requested_cus = txn->requested_cus;
    ulong executed_cus  = txn->executed_cus;
    int   in_block      = !!(txn->flags & FD_TXN_P_FLAGS_EXECUTE_SUCCESS);

    if( FD_UNLIKELY( !in_block && executed_cus>0UL ) ) FD_LOG_ERR(( "Transaction failed execution but consumed CUs?" ));
    if( FD_UNLIKELY( executed_cus>requested_cus    ) ) FD_LOG_ERR(( "Executed (%lu) more than requested (%lu)?", executed_cus, requested_cus ));
    cumulative_block_cost  -= requested_cus - executed_cus;
    cumulative_vote_cost   -= fd_ulong_if( txn->flags & FD_TXN_P_FLAGS_IS_SIMPLE_VOTE, requested_cus-executed_cus, 0UL );
    data_bytes_consumed    -= fd_ulong_if( !in_block,                                  txn->payload_sz,            0UL );
    cumulative_rebated_cus += requested_cus - executed_cus;

    fd_acct_addr_t const * acct = fd_txn_get_acct_addrs( TXN(txn), txn->payload );
    for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( TXN(txn), FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
        iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

      ulong i=fd_txn_acct_iter_idx( iter );

      fd_pack_addr_use_t * in_wcost_table = acct_uses_query( writer_costs, acct[i], NULL );
      if( FD_UNLIKELY( !in_wcost_table ) ) FD_LOG_ERR(( "Rebate to unknown written account" ));
      in_wcost_table->total_cost -= requested_cus - executed_cus;
      /* Important: Even if this is 0, don't delete it from the table so
         that the insert order doesn't get messed up. */
    }
  }

  pack->cumulative_vote_cost   = cumulative_vote_cost;
  pack->cumulative_block_cost  = cumulative_block_cost;
  pack->data_bytes_consumed    = data_bytes_consumed;
  pack->cumulative_rebated_cus = cumulative_rebated_cus;
}


ulong
fd_pack_expire_before( fd_pack_t * pack,
                       ulong       expire_before ) {
  expire_before = fd_ulong_max( expire_before, pack->expire_before );
  ulong deleted_cnt = 0UL;
  fd_pack_expq_t * prq = pack->expiration_q;
  while( (expq_cnt( prq )>0UL) & (prq->expires_at<expire_before) ) {
    fd_pack_ord_txn_t * expired = prq->txn;

    fd_ed25519_sig_t const * expired_sig = fd_txn_get_signatures( TXN( expired->txn ), expired->txn->payload );
    /* fd_pack_delete_transaction also removes it from the heap */
    fd_pack_delete_transaction( pack, expired_sig );
    deleted_cnt++;
  }

  pack->expire_before = expire_before;
  return deleted_cnt;
}

void
fd_pack_end_block( fd_pack_t * pack ) {
  fd_histf_sample( pack->net_cus_per_block,       pack->cumulative_block_cost                                );
  fd_histf_sample( pack->rebated_cus_per_block,   pack->cumulative_rebated_cus                               );
  fd_histf_sample( pack->scheduled_cus_per_block, pack->cumulative_rebated_cus + pack->cumulative_block_cost );

  pack->microblock_cnt         = 0UL;
  pack->data_bytes_consumed    = 0UL;
  pack->cumulative_block_cost  = 0UL;
  pack->cumulative_vote_cost   = 0UL;
  pack->cumulative_rebated_cus = 0UL;

  acct_uses_clear( pack->acct_in_use  );

  if( FD_LIKELY( pack->written_list_cnt<pack->written_list_max-1UL ) ) {
    /* The less dangerous way of doing this is to instead record the
       keys we inserted and do a query followed by a delete for each
       key.  The downside of that is that keys are 32 bytes and a
       pointer is only 8 bytes, plus the computational cost for the
       query.

       However, if we're careful, we can pull this off.  We require two
       things.  First, we started from an empty map and did nothing but
       insert and update.  In particular, no deletions.  Second, we have
       to be careful to delete in the opposite order that we inserted.
       This is essentially like unwinding the inserts we did.  The
       common case is that the element after the one we delete will be
       empty, so we'll hit that case.  It's possible that there's
       another independent probe sequence that will be entirely intact
       starting in the element after, but we'll never hit the MAP_MOVE
       case. */
    for( ulong i=0UL; i<pack->written_list_cnt; i++ ) {
      acct_uses_remove( pack->writer_costs, pack->written_list[ pack->written_list_cnt - 1UL - i ] );
    }
  } else {
    acct_uses_clear( pack->writer_costs );
  }
  pack->written_list_cnt = 0UL;

  FD_PACK_BITSET_CLEAR( pack->bitset_rw_in_use );
  FD_PACK_BITSET_CLEAR( pack->bitset_w_in_use  );

  for( ulong i=0UL; i<pack->bank_tile_cnt; i++ ) pack->use_by_bank_cnt[i] = 0UL;

  /* If our stake is low and we don't become leader often, end_block
     might get called on the order of O(1/hr), which feels too
     infrequent to do anything related to metrics.  However, we only
     update the histograms when we are leader, so this is actually a
     good place to copy them. */
  FD_MHIST_COPY( PACK, TOTAL_TRANSACTIONS_PER_MICROBLOCK_COUNT, pack->txn_per_microblock  );
  FD_MHIST_COPY( PACK, VOTES_PER_MICROBLOCK_COUNT,              pack->vote_per_microblock );

  FD_MGAUGE_SET( PACK, CUS_CONSUMED_IN_BLOCK, 0UL                           );
  FD_MHIST_COPY( PACK, CUS_SCHEDULED,         pack->scheduled_cus_per_block );
  FD_MHIST_COPY( PACK, CUS_REBATED,           pack->rebated_cus_per_block   );
  FD_MHIST_COPY( PACK, CUS_NET,               pack->net_cus_per_block       );
}

static void
release_tree( treap_t           * treap,
              fd_pack_ord_txn_t * pool ) {
  treap_fwd_iter_t next;
  for( treap_fwd_iter_t it=treap_fwd_iter_init( treap, pool ); !treap_fwd_iter_idx( it ); it=next ) {
    next = treap_fwd_iter_next( it, pool );
    ulong idx = treap_fwd_iter_idx( it );
    treap_idx_remove    ( treap, idx, pool );
    trp_pool_idx_release( pool,  idx       );
  }
}

void
fd_pack_clear_all( fd_pack_t * pack ) {
  pack->pending_txn_cnt        = 0UL;
  pack->microblock_cnt         = 0UL;
  pack->cumulative_block_cost  = 0UL;
  pack->cumulative_vote_cost   = 0UL;
  pack->cumulative_rebated_cus = 0UL;

  release_tree( pack->pending,         pack->pool );
  release_tree( pack->pending_votes,   pack->pool );

  expq_remove_all( pack->expiration_q );

  acct_uses_clear( pack->acct_in_use  );
  acct_uses_clear( pack->writer_costs );

  sig2txn_clear( pack->signature_map );

  FD_PACK_BITSET_CLEAR( pack->bitset_rw_in_use );
  FD_PACK_BITSET_CLEAR( pack->bitset_w_in_use  );
  bitset_map_clear( pack->acct_to_bitset );
  pack->bitset_avail[ 0 ] = FD_PACK_BITSET_SLOWPATH;
  for( ulong i=0UL; i<FD_PACK_BITSET_MAX; i++ ) pack->bitset_avail[ i+1UL ] = (ushort)i;
  pack->bitset_avail_cnt = FD_PACK_BITSET_MAX;

  for( ulong i=0UL; i<pack->bank_tile_cnt; i++ ) pack->use_by_bank_cnt[i] = 0UL;
}

int
fd_pack_delete_transaction( fd_pack_t              * pack,
                            fd_ed25519_sig_t const * txn ) {
  fd_pack_sig_to_txn_t * in_tbl = sig2txn_query( pack->signature_map, txn, NULL );

  if( !in_tbl )
    return 0;

  /* The static asserts enforce that the payload of the transaction is
     the first element of the fd_pack_ord_txn_t struct.  The signature
     we insert is 1 byte into the start of the payload. */
  fd_pack_ord_txn_t * containing = (fd_pack_ord_txn_t *)( (uchar*)in_tbl->key - 1UL );
  treap_t * root = NULL;
  int root_idx = containing->root;
  switch( root_idx ) {
    case FD_ORD_TXN_ROOT_FREE:             /* Should be impossible */                                                return 0;
    case FD_ORD_TXN_ROOT_PENDING:          root = pack->pending;                                                     break;
    case FD_ORD_TXN_ROOT_PENDING_VOTE:     root = pack->pending_votes;                                               break;
  }

  fd_txn_t * _txn = TXN( containing->txn );
  fd_acct_addr_t const * accts = fd_txn_get_acct_addrs( _txn, containing->txn->payload );
  for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( _txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
      iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
    ulong i=fd_txn_acct_iter_idx( iter );

    release_result_t ret = release_bit_reference( pack, accts+i );
    FD_PACK_BITSET_CLEARN( pack->bitset_rw_in_use, ret.clear_rw_bit );
    FD_PACK_BITSET_CLEARN( pack->bitset_w_in_use,  ret.clear_w_bit  );
  }

  for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( _txn, FD_TXN_ACCT_CAT_READONLY & FD_TXN_ACCT_CAT_IMM );
      iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
    ulong i=fd_txn_acct_iter_idx( iter );
    if( FD_UNLIKELY( fd_pack_unwritable_contains( accts+i ) ) ) continue;

    release_result_t ret = release_bit_reference( pack, accts+i );
    FD_PACK_BITSET_CLEARN( pack->bitset_rw_in_use, ret.clear_rw_bit );
    FD_PACK_BITSET_CLEARN( pack->bitset_w_in_use,  ret.clear_w_bit  );
  }
  expq_remove( pack->expiration_q, containing->expq_idx );
  treap_ele_remove( root, containing, pack->pool );
  trp_pool_ele_release( pack->pool, containing );
  sig2txn_remove( pack->signature_map, in_tbl );
  pack->pending_txn_cnt--;

  return 1;
}


int
fd_pack_verify( fd_pack_t * pack,
                void      * scratch ) {
  /* Invariants:
     sig2txn_query has exact same contents as all treaps combined
     root matches treap
     Keys of acct_to_bitset is exactly union of all accounts in all
            transactions in treaps, with ref counted appropriately
     bits in bitset_avail is complement of bits allocated in
            acct_to_bitset
     expires_at consistent between treap, prq */

  /* TODO:
     bitset_{r}w_in_use = bitset_map_query( everything in acct_in_use that doesn't have FD_PACK_IN_USE_BIT_CLEARED )
     use_by_bank does not contain duplicates
     use_by_bank consistent with acct_in_use
     bitset_w_in_use & bitset_rw_in_use == bitset_w_in_use */
#define VERIFY_TEST( cond, ... ) do {   \
    if( FD_UNLIKELY( !(cond) ) ) {      \
      FD_LOG_WARNING(( __VA_ARGS__ ));  \
      return -(__LINE__);               \
    }                                   \
  } while( 0 )

  ulong max_acct_in_treap  = pack->pack_depth * FD_TXN_ACCT_ADDR_MAX;
  int lg_acct_in_trp = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_treap  ) );
  void * _bitset_map_copy = scratch;
  void * _bitset_map_orig = bitset_map_leave( pack->acct_to_bitset );
  fd_memcpy( _bitset_map_copy, _bitset_map_orig, bitset_map_footprint( lg_acct_in_trp ) );

  fd_pack_bitset_acct_mapping_t * bitset_copy = bitset_map_join( _bitset_map_copy );

  /* Check that each bit is in exactly one place */
  FD_PACK_BITSET_DECLARE( processed ); FD_PACK_BITSET_CLEAR( processed );
  FD_PACK_BITSET_DECLARE( bit       ); FD_PACK_BITSET_CLEAR( bit       );
  FD_PACK_BITSET_DECLARE( full      ); FD_PACK_BITSET_CLEAR( full      );

  if( FD_UNLIKELY( pack->bitset_avail[0]!=FD_PACK_BITSET_SLOWPATH ) ) return -1;
  for( ulong i=1UL; i<=pack->bitset_avail_cnt; i++ ) {
    FD_PACK_BITSET_CLEAR( bit );
    FD_PACK_BITSET_SETN( bit, pack->bitset_avail[ i ] );
    VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, processed, processed ),
        "bit %hu in avail set twice", pack->bitset_avail[ i ] );
    FD_PACK_BITSET_OR( processed, bit );
  }

  ulong total_references = 0UL;
  for( ulong i=0UL; i<bitset_map_slot_cnt( bitset_copy ); i++ ) {
    if( !bitset_map_key_inval( bitset_copy[ i ].key ) ) {
      VERIFY_TEST( bitset_copy[ i ].ref_cnt>0UL, "account address in table with 0 ref count" );

      total_references += bitset_copy[ i ].ref_cnt;

      FD_PACK_BITSET_CLEAR( bit );
      FD_PACK_BITSET_SETN( bit, bitset_copy[ i ].bit );
      VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, processed, processed ), "bit %hu used twice", bitset_copy[ i ].bit );
      FD_PACK_BITSET_OR( processed, bit );
    }
  }
  for( ulong i=0UL; i<FD_PACK_BITSET_MAX; i++ ) {
    FD_PACK_BITSET_CLEAR( bit );
    FD_PACK_BITSET_SETN( bit, i );
    VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, processed, processed ), "bit %lu missing", i );
    FD_PACK_BITSET_SETN( full, i );
  }


  fd_pack_ord_txn_t  * pool = pack->pool;
  treap_t * treaps[ 2 ] = { pack->pending, pack->pending_votes };
  ulong txn_cnt = 0UL;

  for( ulong k=0UL; k<2; k++ ) {
    treap_t * treap = treaps[ k ];

    for( treap_rev_iter_t _cur=treap_rev_iter_init( treap, pool ); !treap_rev_iter_done( _cur );
        _cur=treap_rev_iter_next( _cur, pool ) ) {
      txn_cnt++;
      fd_pack_ord_txn_t const * cur = treap_rev_iter_ele_const( _cur, pool );
      fd_txn_t const * txn = TXN(cur->txn);
      fd_acct_addr_t const * accts = fd_txn_get_acct_addrs( txn, cur->txn->payload );

      fd_ed25519_sig_t const * sig0 = fd_txn_get_signatures( txn, cur->txn->payload );

      fd_pack_sig_to_txn_t * in_tbl = sig2txn_query( pack->signature_map, sig0, NULL );
      VERIFY_TEST( in_tbl, "signature missing from sig2txn" );
      VERIFY_TEST( in_tbl->key==sig0, "signature in sig2txn inconsistent" );
      VERIFY_TEST( (ulong)(cur->root)==k+1, "treap element had bad root" );
      VERIFY_TEST( cur->expires_at>=pack->expire_before, "treap element expired" );

      fd_pack_expq_t const * eq = pack->expiration_q + cur->expq_idx;
      VERIFY_TEST( eq->txn==cur, "expq inconsistent" );
      VERIFY_TEST( eq->expires_at==cur->expires_at, "expq expires_at inconsistent" );

      FD_PACK_BITSET_DECLARE( complement );
      FD_PACK_BITSET_COPY( complement, full );
      for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
          iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {
        fd_acct_addr_t acct = accts[fd_txn_acct_iter_idx( iter )];

        fd_pack_bitset_acct_mapping_t * q = bitset_map_query( bitset_copy, acct, NULL );
        VERIFY_TEST( q, "account in transaction missing from bitset mapping" );
        VERIFY_TEST( q->ref_cnt>0UL, "account in transaction ref_cnt already 0" );
        q->ref_cnt--;
        total_references--;

        FD_PACK_BITSET_CLEAR( bit );
        FD_PACK_BITSET_SETN( bit, q->bit );
        if( q->bit<FD_PACK_BITSET_MAX ) {
          VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, cur->rw_bitset, cur->rw_bitset ), "missing from rw bitset" );
          VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, cur->w_bitset,  cur->w_bitset ), "missing from w bitset" );
        }
        FD_PACK_BITSET_CLEARN( complement, q->bit );
      }
      VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY( complement, complement, cur->w_bitset,  cur->w_bitset ), "extra in w bitset" );

      for( fd_txn_acct_iter_t iter=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_READONLY & FD_TXN_ACCT_CAT_IMM );
          iter!=fd_txn_acct_iter_end(); iter=fd_txn_acct_iter_next( iter ) ) {

        fd_acct_addr_t acct = accts[fd_txn_acct_iter_idx( iter )];
        if( FD_UNLIKELY( fd_pack_unwritable_contains( &acct ) ) ) continue;
        fd_pack_bitset_acct_mapping_t * q = bitset_map_query( bitset_copy, acct, NULL );
        VERIFY_TEST( q, "account in transaction missing from bitset mapping" );
        VERIFY_TEST( q->ref_cnt>0UL, "account in transaction ref_cnt already 0" );
        q->ref_cnt--;
        total_references--;

        FD_PACK_BITSET_CLEAR( bit );
        FD_PACK_BITSET_SETN( bit, q->bit );
        if( q->bit<FD_PACK_BITSET_MAX ) {
          VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, cur->rw_bitset, cur->rw_bitset ), "missing from rw bitset" );
        }
        FD_PACK_BITSET_CLEARN( complement, q->bit );
      }
      VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY( complement, complement, cur->rw_bitset,  cur->rw_bitset ), "extra in rw bitset" );
    }
  }

  bitset_map_leave( bitset_copy );

  VERIFY_TEST( total_references==0UL, "extra references in bitset mapping" );
  VERIFY_TEST( txn_cnt==sig2txn_key_cnt( pack->signature_map ), "extra signatures in sig2txn" );

  bitset_map_join( _bitset_map_orig );

  ulong max_acct_in_flight = pack->bank_tile_cnt * (FD_TXN_ACCT_ADDR_MAX * pack->lim->max_txn_per_microblock + 1UL);
  int lg_uses_tbl_sz = fd_ulong_find_msb( fd_ulong_pow2_up( 2UL*max_acct_in_flight ) );

  void * _acct_in_use_copy = scratch;
  void * _acct_in_use_orig = acct_uses_leave( pack->acct_in_use );
  fd_memcpy( _acct_in_use_copy, _acct_in_use_orig, acct_uses_footprint( lg_uses_tbl_sz ) );

  fd_pack_addr_use_t * acct_in_use_copy = acct_uses_join( _acct_in_use_copy );

  FD_PACK_BITSET_DECLARE(  w_complement );
  FD_PACK_BITSET_DECLARE( rw_complement );
  FD_PACK_BITSET_COPY(  w_complement, full );
  FD_PACK_BITSET_COPY( rw_complement, full );

  FD_PACK_BITSET_DECLARE( rw_bitset );  FD_PACK_BITSET_COPY( rw_bitset, pack->bitset_rw_in_use );
  FD_PACK_BITSET_DECLARE(  w_bitset );  FD_PACK_BITSET_COPY(  w_bitset, pack->bitset_w_in_use  );


  ulong const EMPTY_MASK = ~(FD_PACK_IN_USE_WRITABLE | FD_PACK_IN_USE_BIT_CLEARED);

  for( ulong bank=0UL; bank<pack->bank_tile_cnt; bank++ ) {

    fd_pack_addr_use_t const * base = pack->use_by_bank[ bank ];
    ulong bank_mask = 1UL << bank;

    for( ulong i=0UL; i<pack->use_by_bank_cnt[ bank ]; i++ ) {
      fd_pack_addr_use_t * use = acct_uses_query( acct_in_use_copy, base[i].key, NULL );
      VERIFY_TEST( use, "acct in use by bank not in acct_in_use, or in uses_by_bank twice" );

      VERIFY_TEST( use->in_use_by & bank_mask, "acct in uses_by_bank doesn't have corresponding bit set in acct_in_use, or it was in the list twice" );

      fd_pack_bitset_acct_mapping_t * q = bitset_map_query( pack->acct_to_bitset, base[i].key, NULL );
      /* The normal case is that the acct->bit mapping is preserved
         while in use by other transactions in the pending list.  This
         might not always happen though.  It's okay for the mapping to
         get deleted while the acct is in use, which is noted with
         BIT_CLEARED.  If that is set, the mapping may not exist, or it
         may have been re-created, perhaps with a different bit. */
      if( q==NULL ) VERIFY_TEST( use->in_use_by & FD_PACK_IN_USE_BIT_CLEARED, "acct in use not in acct_to_bitset, but not marked as cleared" );
      else if( !(use->in_use_by & FD_PACK_IN_USE_BIT_CLEARED) ) {
        FD_PACK_BITSET_CLEAR( bit );
        FD_PACK_BITSET_SETN( bit, q->bit );
        if( q->bit<FD_PACK_BITSET_MAX ) {
          VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, rw_bitset, rw_bitset ), "missing from rw bitset" );
          if( use->in_use_by & FD_PACK_IN_USE_WRITABLE ) {
            VERIFY_TEST( !FD_PACK_BITSET_INTERSECT4_EMPTY( bit, bit, w_bitset, w_bitset ), "missing from w bitset" );
            FD_PACK_BITSET_CLEARN( w_complement, q->bit );
          }
        }
        FD_PACK_BITSET_CLEARN( rw_complement, q->bit );
      }
      if( use->in_use_by & FD_PACK_IN_USE_WRITABLE ) VERIFY_TEST( (use->in_use_by & EMPTY_MASK)==bank_mask, "writable, but in use by multiple" );

      use->in_use_by &= ~bank_mask;
      if( !(use->in_use_by & EMPTY_MASK) ) acct_uses_remove( acct_in_use_copy, use );
    }
  }
  VERIFY_TEST( acct_uses_key_cnt( acct_in_use_copy )==0UL, "stray uses in acct_in_use" );
  VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY( rw_complement, rw_complement, rw_bitset,  rw_bitset ), "extra in rw bitset" );
  VERIFY_TEST( FD_PACK_BITSET_INTERSECT4_EMPTY(  w_complement,  w_complement,  w_bitset,   w_bitset ), "extra in w bitset" );

  acct_uses_leave( acct_in_use_copy );

  acct_uses_join( _acct_in_use_orig );
  return 0;
}

void * fd_pack_leave ( fd_pack_t * pack ) { FD_COMPILER_MFENCE(); return (void *)pack; }
void * fd_pack_delete( void      * mem  ) { FD_COMPILER_MFENCE(); return mem;          }
