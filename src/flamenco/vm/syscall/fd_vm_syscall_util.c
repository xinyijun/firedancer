#include "fd_vm_syscall.h"

#include "../../../ballet/base64/fd_base64.h"
#include "../../runtime/sysvar/fd_sysvar.h"
#include "../../runtime/sysvar/fd_sysvar_clock.h"
#include "../../runtime/sysvar/fd_sysvar_epoch_schedule.h"
#include "../../runtime/sysvar/fd_sysvar_fees.h"
#include "../../runtime/context/fd_exec_txn_ctx.h"
#include "../../runtime/context/fd_exec_instr_ctx.h"
#include "../../runtime/fd_account.h"

int
fd_vm_syscall_abort( FD_PARAM_UNUSED void *  _vm,
                     FD_PARAM_UNUSED ulong   r1,
                     FD_PARAM_UNUSED ulong   r2,
                     FD_PARAM_UNUSED ulong   r3,
                     FD_PARAM_UNUSED ulong   r4,
                     FD_PARAM_UNUSED ulong   r5,
                     FD_PARAM_UNUSED ulong * _ret ) {

  /* FIXME: this originally cleared *_ret, which would change r0 to 0 as
     part of the abort.  This is commented out below to preserve VM
     state at precisely the time of the abort (including the updaets to
     ic and cu for the syscall itself).  It is trivial to flip back if
     desired by uncommenting the below. */

//*_ret = 0;

  return FD_VM_ERR_ABORT;
}

int
fd_vm_syscall_sol_panic( /**/            void *  _vm,
                         /**/            ulong   msg_vaddr,
                         /**/            ulong   msg_sz,
                         FD_PARAM_UNUSED ulong   r3,
                         FD_PARAM_UNUSED ulong   r4,
                         FD_PARAM_UNUSED ulong   r5,
                         FD_PARAM_UNUSED ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  /* To avoid a DOS attack from the transactions calling panic with large
     messages, we just append the message to the log caller (like
     suggested in a pre-belt sanding TODO).  We defer to any
     runtime handler of the syscall log UTF-8 validation, checking for
     proper cstr termination, etc.  While we don't strictly need to
     check the compute units here, it is a fast O(1) and can thus avoid
     a large memcpy to further keep performance reasonable. */

  FD_VM_CU_UPDATE( vm, msg_sz );
  fd_vm_log_append( vm, FD_VM_MEM_SLICE_HADDR_LD( vm, msg_vaddr, 1UL, msg_sz ), msg_sz );

  return FD_VM_ERR_PANIC;
}

int
fd_vm_syscall_sol_log( /**/            void *  _vm,
                       /**/            ulong   msg_vaddr,
                       /**/            ulong   msg_sz,
                       FD_PARAM_UNUSED ulong   r2,
                       FD_PARAM_UNUSED ulong   r3,
                       FD_PARAM_UNUSED ulong   r4,
                       /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  /* FIXME: should this do things like UTF validation or what not and
     fail the transaction for syscall cases that currently otherwise
     return success? */

  FD_VM_CU_UPDATE( vm, fd_ulong_max( msg_sz, FD_VM_SYSCALL_BASE_COST ) );

  /* https://github.com/anza-xyz/agave/blob/ba9bf247c312a7f5e309650f921d1e0e8e741fde/programs/bpf_loader/src/syscalls/logging.rs#L21-L30 */
  const void * msg_content = FD_VM_MEM_SLICE_HADDR_LD( vm, msg_vaddr, 1UL, msg_sz );
  fd_vm_log_append( vm, "Program log: ", 13UL );
  fd_vm_log_append( vm, msg_content, msg_sz );

  *_ret = 0UL;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_log_64( void *  _vm,
                          ulong   r1,
                          ulong   r2,
                          ulong   r3,
                          ulong   r4,
                          ulong   r5,
                          ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_UPDATE( vm, FD_VM_LOG_64_UNITS );

  /* Note: The original version of this use sprintf to a stack buffer
     and then copied the result into the log, not including the '\0'
     termination.  This does the printf directly into the log buffer.
     The tail region is large enough (128) to handle the worst case msg
     (13+16*5+4+1).  Since only the strlen bytes of the message is
     actually published to the log and the log API explicitly allows
     message producers to clobber the entire log preparation region,
     this replicates the old log behavior exactly published message
     bytes but potentially has different bytes in the clobber region. */

  /* FIXME: consider even lower overhead pretty printing? */
  char * msg     = (char *)fd_vm_log_prepare( vm );
  ulong  msg_max = fd_vm_log_prepare_max( vm );
  ulong  msg_len;

  fd_cstr_printf( msg, msg_max, &msg_len, "Program log: 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx",
                  r1, r2, r3, r4, r5 );

  fd_vm_log_publish( vm, msg_len );

  *_ret = 0UL;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_log_pubkey( /**/            void *  _vm,
                              /**/            ulong   pubkey_vaddr,
                              FD_PARAM_UNUSED ulong   r2,
                              FD_PARAM_UNUSED ulong   r3,
                              FD_PARAM_UNUSED ulong   r4,
                              FD_PARAM_UNUSED ulong   r5,
                              /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_UPDATE( vm, FD_VM_LOG_PUBKEY_UNITS );
  void const * pubkey = FD_VM_MEM_HADDR_LD( vm, pubkey_vaddr, 1UL, sizeof(fd_pubkey_t) );

  /* Note that prepare_max is guaranteed large enough (128) to handle
     the worst case len here (13+44+1).  See note in sol_log above about
     tail clobbering. */

  char * msg = (char *)fd_vm_log_prepare( vm );
  ulong  msg_len;

  char * p = fd_cstr_init( msg );
  p = fd_cstr_append_text( p, "Program log: ", 13UL );
  ulong  pubkey_len; fd_base58_encode_32( pubkey, &pubkey_len, p ); p += pubkey_len;
  msg_len = (ulong)(p - msg);
  fd_cstr_fini( p );

  fd_vm_log_publish( vm, msg_len );

  *_ret = 0UL;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_log_compute_units( /**/            void *  _vm,
                                     FD_PARAM_UNUSED ulong   r1,
                                     FD_PARAM_UNUSED ulong   r2,
                                     FD_PARAM_UNUSED ulong   r3,
                                     FD_PARAM_UNUSED ulong   r4,
                                     FD_PARAM_UNUSED ulong   r5,
                                     /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_UPDATE( vm, FD_VM_SYSCALL_BASE_COST );

  /* At this point, vm->cu is the remaining compute units between this
     syscall and the following instruction. */

  /* See note in sol_log above about tail clobbering. */

  char * msg     = (char *)fd_vm_log_prepare( vm );
  ulong  msg_max = fd_vm_log_prepare_max( vm );
  ulong  msg_len;

  fd_cstr_printf( msg, msg_max, &msg_len, "Program consumption: %lu units remaining", vm->cu );

  fd_vm_log_publish( vm, msg_len );

  *_ret = 0UL;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_log_data( /**/            void *  _vm,
                            /**/            ulong   slice_vaddr,
                            /**/            ulong   slice_cnt,
                            FD_PARAM_UNUSED ulong   r3,
                            FD_PARAM_UNUSED ulong   r4,
                            FD_PARAM_UNUSED ulong   r5,
                            /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  /* Make sure we have enough compute budget and every address range is
     valid before we do any work to avoid DOS risk from a malicious
     syscall that has a big slice count, a bunch of valid slices to
     trigger a lot of work but then faults on the last slice. */

  FD_VM_CU_UPDATE( vm, FD_VM_SYSCALL_BASE_COST );

  if( FD_UNLIKELY( slice_cnt>(ULONG_MAX/sizeof(fd_vm_vec_t)) ) ) return FD_VM_ERR_SIGSEGV; /* FIXME: SIGOVERFLOW maybe? */
  ulong slice_sz = slice_cnt*sizeof(fd_vm_vec_t);

  fd_vm_vec_t const * slice = (fd_vm_vec_t const *)FD_VM_MEM_SLICE_HADDR_LD( vm, slice_vaddr, FD_VM_VEC_ALIGN, slice_sz );

  /* https://github.com/firedancer-io/solana/blob/06ec63044892e5ee14b6fa15d8c55da9953d0c09/programs/bpf_loader/src/syscalls/logging.rs#L135 */
  FD_VM_CU_UPDATE( vm, fd_ulong_sat_mul( FD_VM_SYSCALL_BASE_COST, slice_cnt ) );
  for( ulong slice_idx=0UL; slice_idx<slice_cnt; slice_idx++ ) {
    FD_VM_CU_UPDATE( vm, slice[slice_idx].len );
  }

  /* Call is guaranteed to succeed at this point */

  fd_vm_log_append( vm, "Program data: ", 14UL );

  for( ulong slice_idx=0UL; slice_idx<slice_cnt; slice_idx++ ) {
    if( FD_UNLIKELY( !fd_vm_log_rem( vm ) ) ) break; /* If the log is at limit, don't waste time on fully discarded messages */

    /* Note that buf_sz requires:

         FD_BASE64_ENC_SZ( buf_sz ) == 4 ceil( buf_sz/3 )

       to encode.  This might be larger than msg_max-1 (note that we
       also usually need to encode a space after the message).  We thus
       want a safe maximum we can encode into msg_max-1 space; this is
       similar to but not quite the same as FD_BASE64_DEC_SZ(msg_max-1).

       That is, we want a buf_lim such that:

            4 ceil( buf_lim/3 ) <= (msg_max-1)
         ->   ceil( buf_lim/3 ) <= (msg_max-1)/4

       Noting that, for integral buf_lim:

         ceil( buf_lim/3 ) == floor( (buf_lim+2)/3 ) <= (buf_lim+2)/3

       Thus, buf_lim is guaranteed safe if:

            (buf_lim+2)/3 <= (msg_max-1)/4
         -> buf_lim <= (3*(msg_max-1)/4)-2 == (3*msg_max-11)/4

       Since buf_lim is integral, this implies a safe maximum to encode
       is:

             floor( (3*msg_max-11)/4 )

       This is not necessarily the largest possible value but it will
       be really close and the tail clobbering region in msg_max will
       naturally give enough margin such that all buffer bytes that can
       be encoded into the log will be whether or not this is tight
       and/or we need to append a space.  We don't have to worry about
       overflow with the multiplication because msg_max<<ULONG_MAX.  We
       likewise don't have to worry about underflow from the
       subtractions because msg_max>>4 due to LOG_TAIL. */

    char * msg     = (char *)fd_vm_log_prepare( vm );
    ulong  msg_max = fd_vm_log_prepare_max( vm );
    ulong msg_len  = 0UL;
    if ( FD_LIKELY( slice[ slice_idx ].len > 0UL ) ) {
      msg_len = fd_base64_encode( msg,
                                       FD_VM_MEM_HADDR_LD_FAST( vm, slice[ slice_idx ].addr ),
                                       fd_ulong_min( slice[ slice_idx ].len, (3UL*msg_max-11UL)/4UL ) );
    }
    msg[ msg_len ] = ' ';
    msg_len += (ulong)( slice_idx < (slice_cnt-1UL) ); /* Note that slice cnt is at least 1 here */
    fd_vm_log_publish( vm, msg_len );
  }

  *_ret = 0;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_alloc_free( /**/            void *  _vm,
                              /**/            ulong   sz,
                              /**/            ulong   free_vaddr,
                              FD_PARAM_UNUSED ulong   r3,
                              FD_PARAM_UNUSED ulong   r4,
                              FD_PARAM_UNUSED ulong   r5,
                              /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  /* This syscall is ... uh ... problematic.  But the community has
     already recognized this and deprecated it:

     https://github.com/solana-labs/solana/blob/v1.17.23/sdk/src/feature_set.rs#L846

     Unfortunately, old code never dies so, practically, this will need
     to be supported until the heat death of the universe.

     The most serious issue is that there is nothing to stop VM code
     making a decision based on the _location_ of the returned
     allocation.  If different validator implementations use different
     allocator algorithms, though each implementation would behave
     functionally correct in isolation, the VM code that uses it would
     actually break consensus.

     As a result, every validator needs to use a bit-for-bit identical
     allocation algorithm.  Fortunately, Solana is just using a basic
     bump allocator:

     https://github.com/solana-labs/solana/blob/v1.17.23/program-runtime/src/invoke_context.rs#L122-L148

     vm->heap_{sz,max} and the below replicate this exactly.

     Another major issue is that this alloc doesn't always conform
     typical malloc/free semantics (e.g. C/C++ requires malloc to have
     an alignment safe for primitive types ... 8 for the Solana machine
     model).  This is clearly to support backward compat with older VM
     code (though ideally a malloc syscall should have behaved like ...
     well ... malloc from day 1).  So the alignment behavior below is a
     bug-for-bug replication of that:

     https://github.com/solana-labs/solana/blob/v1.17.23/programs/bpf_loader/src/syscalls/mod.rs#L645-L681
     https://github.com/solana-labs/solana/blob/v1.17.23/sdk/program/src/entrypoint.rs#L265-L266

     More generally and already ranted about elsewhere, any code that
     uses malloc/free style dynamic allocation is inherently broken.  So
     this syscall should have never existed in the first place ... it
     just feeds the trolls.  The above is just additional implementation
     horror because people consistent think malloc/free is much simpler
     than it actually is.  This is also an example of how quickly
     mistakes fossilize and become a thorn-in-the-side forever.

     IMPORTANT SAFETY TIP!  heap_start must be non zero and both
     heap_start and heap_end should have an alignment of at least 8.
     This existing runtime policies around heap implicitly satisfy this.

     IMPORTANT SAFETY TIP!  The specification for Rust's align_offset
     doesn't seem to provide a strong guarantee that it will return the
     minimal positive offset necessary to align pointers.  It is
     possible for a "conforming" Rust compiler to break consensus by
     using a different align_offset implementation that aligned pointer
     between different compilations of the Solana validator and the
     below. */

  /* Non-zero free address implies that this is a free() call.  Since
     this is a bump allocator, free is a no-op. */

  if( FD_UNLIKELY( free_vaddr ) ) {
    *_ret = 0UL;
    return FD_VM_SUCCESS;
  }

  ulong align = vm->check_align ? 8UL : 1UL;

  ulong heap_sz    = fd_ulong_align_up( vm->heap_sz, align                           );
  ulong heap_vaddr = fd_ulong_sat_add ( heap_sz,     FD_VM_MEM_MAP_HEAP_REGION_START );
  /**/  heap_sz    = fd_ulong_sat_add ( heap_sz,     sz                              );

  if( FD_UNLIKELY( heap_sz > vm->heap_max ) ) { /* Not enough free memory */
    *_ret = 0UL;
    return FD_VM_SUCCESS;
  }

  vm->heap_sz = heap_sz;

  *_ret = heap_vaddr;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_memcpy( /**/            void *  _vm,
                          /**/            ulong   dst_vaddr,
                          /**/            ulong   src_vaddr,
                          /**/            ulong   sz,
                          FD_PARAM_UNUSED ulong   r4,
                          FD_PARAM_UNUSED ulong   r5,
                          /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  /* FIXME: check cu model (this model seems to be using CPI related
     parameters!) */
  /* FIXME: confirm exact handling matches Solana for the NULL, sz==0
     and/or dst==src cases (see other mem syscalls ... they don't all
     fault in the same way though in principle that shouldn't break
     consensus).  Except for fixing the overflow risk from wrapping
     ranges (the below is computed as though the ranges are in exact
     math and don't overlap), the below handling matches the original
     implementation. */
  /* FIXME: consider what err code to use on overlap case further */
  /* FIXME: use overlap logic from runtime? */

  FD_VM_CU_MEM_OP_UPDATE( vm, sz );

  if( FD_UNLIKELY( ((src_vaddr> dst_vaddr) & ((src_vaddr-dst_vaddr)<sz)) |
                   ((dst_vaddr>=src_vaddr) & ((dst_vaddr-src_vaddr)<sz)) ) ) return FD_VM_ERR_MEM_OVERLAP;

  if( FD_UNLIKELY( !sz ) ) {
    *_ret = 0;
    return FD_VM_SUCCESS;
  }

  if( !FD_FEATURE_ACTIVE( vm->instr_ctx->slot_ctx, bpf_account_data_direct_mapping ) ) {
    void *       dst = FD_VM_MEM_HADDR_ST( vm, dst_vaddr, 1UL, sz );
    void const * src = FD_VM_MEM_HADDR_LD( vm, src_vaddr, 1UL, sz );

    fd_memcpy( dst, src, sz );

    *_ret = 0;
    return FD_VM_SUCCESS;
  } else {

    /* Lookup host address chunks.  Try to do a standard memcpy if the regions
       do not cross memory regions. */
    ulong   dst_region              = dst_vaddr >> 32;
    ulong   dst_offset              = dst_vaddr & 0xffffffffUL;
    ulong   dst_region_idx          = 0UL;
    ulong   dst_bytes_in_cur_region = sz;
    uchar * dst_haddr               = NULL;
    if( dst_region==4UL ) {
      dst_region_idx          = fd_vm_get_input_mem_region_idx( vm, dst_offset );
      dst_haddr               = (uchar*)(vm->input_mem_regions[ dst_region_idx ].haddr + dst_offset - vm->input_mem_regions[ dst_region_idx ].vaddr_offset);
      dst_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ dst_region_idx ].region_sz - 
                                                  ((ulong)dst_haddr - vm->input_mem_regions[ dst_region_idx ].haddr) );
      if( FD_UNLIKELY( !vm->input_mem_regions[ dst_region_idx ].is_writable ) ) {
        *_ret = 1;
        return FD_VM_ERR_ABORT;
      }
    } else {
      dst_haddr = (uchar *)FD_VM_MEM_SLICE_HADDR_ST( vm, dst_vaddr, 1UL, sz );
    }

    ulong src_region              = src_vaddr >> 32;
    ulong src_offset              = src_vaddr & 0xffffffffUL;
    ulong src_region_idx          = 0UL;
    ulong src_bytes_in_cur_region = sz;
    uchar * src_haddr             = NULL;
    if( src_region==4UL ) {
      src_region_idx          = fd_vm_get_input_mem_region_idx( vm, src_offset );
      src_haddr               = (uchar*)(vm->input_mem_regions[ src_region_idx ].haddr + src_offset - vm->input_mem_regions[ src_region_idx ].vaddr_offset);
      src_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ src_region_idx ].region_sz - 
                                                  ((ulong)src_haddr - vm->input_mem_regions[ src_region_idx ].haddr) );
    } else {
      src_haddr           = (uchar *)FD_VM_MEM_SLICE_HADDR_LD( vm, src_vaddr, 1UL, sz );
    }

    /* Do a normal memcpy if regions do not overlap */
    if( FD_LIKELY( src_bytes_in_cur_region==dst_bytes_in_cur_region && src_bytes_in_cur_region==sz ) ) {
      fd_memcpy( dst_haddr, src_haddr, sz );
      *_ret = 0;
      return FD_VM_SUCCESS;
    }
  
    /* Case where the operation spans multiple regions. Copy over the bytes
       from each region while iterating to the next one. */
    /* TODO: An optimization would be to memcpy chunks at once */
    ulong dst_idx = 0UL;
    ulong src_idx = 0UL;
    for( ulong i=0UL; i<sz; i++ ) {
      if( FD_UNLIKELY( !dst_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++dst_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        if( FD_UNLIKELY( !vm->input_mem_regions[ dst_region_idx ].is_writable ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        dst_haddr = (uchar*)vm->input_mem_regions[ dst_region_idx ].haddr;
        dst_idx = 0UL;
      }
      if( FD_UNLIKELY( !src_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++src_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        src_haddr = (uchar*)vm->input_mem_regions[ src_region_idx ].haddr;
        src_idx = 0UL;
      }

      dst_haddr[ dst_idx ] = src_haddr[ src_idx ];

      dst_bytes_in_cur_region--;
      src_bytes_in_cur_region--;
      dst_idx++;
      src_idx++;
    }
    *_ret = 0;
    return FD_VM_SUCCESS;
  }
}

int
fd_vm_syscall_sol_memcmp( /**/            void *  _vm,
                          /**/            ulong   m0_vaddr,
                          /**/            ulong   m1_vaddr,
                          /**/            ulong   sz,
                          /**/            ulong   out_vaddr,
                          FD_PARAM_UNUSED ulong   r5,
                          /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_MEM_OP_UPDATE( vm, sz );

  /* Note: though this behaves like a normal C-style memcmp, we can't
     use the compilers / libc memcmp directly because the specification
     doesn't provide strong enough guarantees about the return value (it
     only promises the sign). */

  if( !FD_FEATURE_ACTIVE( vm->instr_ctx->slot_ctx, bpf_account_data_direct_mapping ) ) {
    uchar const * m0 = (uchar const *)FD_VM_MEM_SLICE_HADDR_LD( vm, m0_vaddr, 1UL, sz );
    uchar const * m1 = (uchar const *)FD_VM_MEM_SLICE_HADDR_LD( vm, m1_vaddr, 1UL, sz );

    /* Silly that this doesn't use r0 to return ... slower, more edge
      case, different from libc style memcmp, harder to callers to use,
      etc ... probably too late to do anything about it now ... sigh */

    void * _out = FD_VM_MEM_HADDR_ST( vm, out_vaddr, 4UL, 4UL );

    int out = 0;
    for( ulong i=0UL; i<sz; i++ ) {
      int i0 = (int)m0[i];
      int i1 = (int)m1[i];
      if( i0!=i1 ) {
        out = i0 - i1;
        break;
      }
    }

    fd_memcpy( _out, &out, 4UL ); /* Sigh ... see note above (and might be unaligned ... double sigh) */

    *_ret = 0;
    return FD_VM_SUCCESS;
  } else {
    void * _out = FD_VM_MEM_HADDR_ST( vm, out_vaddr, 4UL, 4UL );

    int    out  = 0;
    /* Lookup host address chunks.  Try to do a standard memcpy if the regions
       do not cross memory regions. */
    ulong   m0_region              = m0_vaddr >> 32;
    ulong   m0_offset              = m0_vaddr & 0xffffffffUL;
    ulong   m0_region_idx          = 0UL;
    ulong   m0_bytes_in_cur_region = sz;
    uchar * m0_haddr               = NULL;
    if( m0_region==4UL ) {
      m0_region_idx          = fd_vm_get_input_mem_region_idx( vm, m0_offset );
      m0_haddr               = (uchar*)(vm->input_mem_regions[ m0_region_idx ].haddr + m0_offset - vm->input_mem_regions[ m0_region_idx ].vaddr_offset);
      m0_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ m0_region_idx ].region_sz - 
                                                  ((ulong)m0_haddr - vm->input_mem_regions[ m0_region_idx ].haddr) );
    } else {
      m0_haddr = (uchar *)FD_VM_MEM_SLICE_HADDR_LD( vm, m0_vaddr, 1UL, sz );
    }

    ulong   m1_region              = m1_vaddr >> 32;
    ulong   m1_offset              = m1_vaddr & 0xffffffffUL;
    ulong   m1_region_idx          = 0UL;
    ulong   m1_bytes_in_cur_region = sz;
    uchar * m1_haddr               = NULL;
    if( m1_region==4UL ) {
      m1_region_idx          = fd_vm_get_input_mem_region_idx( vm, m1_offset );
      m1_haddr               = (uchar*)(vm->input_mem_regions[ m1_region_idx ].haddr + m1_offset - vm->input_mem_regions[ m1_region_idx ].vaddr_offset);
      m1_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ m1_region_idx ].region_sz - 
                                                  ((ulong)m1_haddr - vm->input_mem_regions[ m1_region_idx ].haddr) );
    } else {
      m1_haddr = (uchar *)FD_VM_MEM_SLICE_HADDR_LD( vm, m1_vaddr, 1UL, sz );
    }

    /* Case where the operation spans multiple regions. Copy over the bytes
       from each region while iterating to the next one. */
    /* TODO: An optimization would be to memcpy chunks at once */
    ulong m0_idx = 0UL;
    ulong m1_idx = 0UL;
    for( ulong i=0UL; i<sz; i++ ) {
      if( FD_UNLIKELY( !m0_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++m0_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        m0_haddr = (uchar*)vm->input_mem_regions[ m0_region_idx ].haddr;
        m0_idx = 0UL;
      }
      if( FD_UNLIKELY( !m1_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++m1_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        m1_haddr = (uchar*)vm->input_mem_regions[ m1_region_idx ].haddr;
        m1_idx = 0UL;
      }

      int i0 = (int)m0_haddr[ m0_idx ];
      int i1 = (int)m1_haddr[ m1_idx ];
      if( i0!=i1 ) {
        out = i0 - i1;
        break;
      }

      m0_bytes_in_cur_region--;
      m1_bytes_in_cur_region--;
      m0_idx++;
      m1_idx++;
    }
    fd_memcpy( _out, &out, 4UL ); /* Sigh ... see note above (and might be unaligned ... double sigh) */
    *_ret = 0;
    return FD_VM_SUCCESS;
  }
}

int
fd_vm_syscall_sol_memset( /**/            void *  _vm,
                          /**/            ulong   dst_vaddr,
                          /**/            ulong   c,
                          /**/            ulong   sz,
                          FD_PARAM_UNUSED ulong   r4,
                          FD_PARAM_UNUSED ulong   r5,
                          /**/            ulong * _ret ) {
  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_MEM_OP_UPDATE( vm, sz );

  ulong FD_FN_UNUSED dst_region = dst_vaddr >> 32;
  int b = (int)(c & 255UL);

  if( dst_region!=4UL || !FD_FEATURE_ACTIVE( vm->instr_ctx->slot_ctx, bpf_account_data_direct_mapping ) ) {
    void * dst = FD_VM_MEM_SLICE_HADDR_ST( vm, dst_vaddr, 1UL, sz );
    if( FD_LIKELY( sz ) ) {
      memset( dst, b, sz );
    }
  } else {
    /* Syscall manages the pointer accesses directly and will report in the 
       case of bad memory accesses. */
    ulong sz_left              = sz;
    ulong dst_offset           = dst_vaddr & 0xffffffffUL;
    ulong region_idx           = fd_vm_get_input_mem_region_idx( vm, dst_offset );
    ulong region_offset        = dst_offset - vm->input_mem_regions[region_idx].vaddr_offset;
    ulong bytes_left_in_region = vm->input_mem_regions[region_idx].region_sz - region_offset;
    uchar * haddr              = (uchar*)(vm->input_mem_regions[region_idx].haddr + region_offset);

    while( sz_left ) {
      if( FD_UNLIKELY( region_idx>=vm->input_mem_regions_cnt ) ) {
        *_ret = 1UL;
        return FD_VM_ERR_INVAL;
      }
      if( FD_UNLIKELY( !vm->input_mem_regions[region_idx].is_writable ) ) {
        *_ret = 1UL;
        return FD_VM_ERR_INVAL;
      }

      ulong bytes_to_write = fd_ulong_min( sz_left, bytes_left_in_region );
      memset( haddr, b, bytes_to_write );

      sz_left = fd_ulong_sat_sub( sz_left, bytes_to_write );
      region_idx++;

      if( region_idx!=vm->input_mem_regions_cnt ) {
        haddr                = (uchar*)vm->input_mem_regions[region_idx].haddr;
        bytes_left_in_region = vm->input_mem_regions[region_idx].region_sz;
      }
    }
  }

  *_ret = 0;
  return FD_VM_SUCCESS;
}

int
fd_vm_syscall_sol_memmove( /**/            void *  _vm,
                           /**/            ulong   dst_vaddr,
                           /**/            ulong   src_vaddr,
                           /**/            ulong   sz,
                           FD_PARAM_UNUSED ulong   r4,
                           FD_PARAM_UNUSED ulong   r5,
                           /**/            ulong * _ret ) {

  fd_vm_t * vm = (fd_vm_t *)_vm;

  FD_VM_CU_MEM_OP_UPDATE( vm, sz );

  if( !FD_FEATURE_ACTIVE( vm->instr_ctx->slot_ctx, bpf_account_data_direct_mapping ) ) {
    void *       dst = FD_VM_MEM_SLICE_HADDR_ST( vm, dst_vaddr, 1UL, sz );
    void const * src = FD_VM_MEM_SLICE_HADDR_LD( vm, src_vaddr, 1UL, sz );
    if( FD_LIKELY( sz > 0 ) ) {
      memmove( dst, src, sz );
    }
  } else {
    /* Lookup host address chunks.  Try to do a standard memcpy if the regions
       do not cross memory regions. */
    ulong   dst_region              = dst_vaddr >> 32;
    ulong   dst_offset              = dst_vaddr & 0xffffffffUL;
    ulong   dst_region_idx          = 0UL;
    ulong   dst_bytes_in_cur_region = sz;
    uchar * dst_haddr               = NULL;
    if( dst_region==4UL ) {
      dst_region_idx          = fd_vm_get_input_mem_region_idx( vm, dst_offset );
      dst_haddr               = (uchar*)(vm->input_mem_regions[ dst_region_idx ].haddr + dst_offset - vm->input_mem_regions[ dst_region_idx ].vaddr_offset);
      dst_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ dst_region_idx ].region_sz - 
                                                  ((ulong)dst_haddr - vm->input_mem_regions[ dst_region_idx ].haddr) );
      if( FD_UNLIKELY( !vm->input_mem_regions[ dst_region_idx ].is_writable ) ) {
        *_ret = 1;
        return FD_VM_ERR_ABORT;
      }
    } else {
      dst_haddr = (uchar *)FD_VM_MEM_SLICE_HADDR_ST( vm, dst_vaddr, 1UL, sz );
    }

    ulong src_region              = src_vaddr >> 32;
    ulong src_offset              = src_vaddr & 0xffffffffUL;
    ulong src_region_idx          = 0UL;
    ulong src_bytes_in_cur_region = sz;
    uchar * src_haddr             = NULL;
    if( src_region==4UL ) {
      src_region_idx          = fd_vm_get_input_mem_region_idx( vm, src_offset );
      src_haddr               = (uchar*)(vm->input_mem_regions[ src_region_idx ].haddr + src_offset - vm->input_mem_regions[ src_region_idx ].vaddr_offset);
      src_bytes_in_cur_region = fd_ulong_min( sz, vm->input_mem_regions[ src_region_idx ].region_sz - 
                                                  ((ulong)src_haddr - vm->input_mem_regions[ src_region_idx ].haddr) );
    } else {
      src_haddr           = (uchar *)FD_VM_MEM_SLICE_HADDR_LD( vm, src_vaddr, 1UL, sz );
    }

    /* Do a normal memcpy if regions do not overlap */
    if( FD_LIKELY( src_bytes_in_cur_region==dst_bytes_in_cur_region && src_bytes_in_cur_region==sz ) ) {
      memmove( dst_haddr, src_haddr, sz );
      *_ret = 0;
      return FD_VM_SUCCESS;
    }
  
    /* Case where the operation spans multiple regions. Copy over the bytes
       from each region while iterating to the next one. */
    /* TODO: An optimization would be to memcpy chunks at once */
    ulong dst_idx = 0UL;
    ulong src_idx = 0UL;
    for( ulong i=0UL; i<sz; i++ ) {
      if( FD_UNLIKELY( !dst_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++dst_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        if( FD_UNLIKELY( !vm->input_mem_regions[ dst_region_idx ].is_writable ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        dst_haddr = (uchar*)vm->input_mem_regions[ dst_region_idx ].haddr;
        dst_idx = 0UL;
      }
      if( FD_UNLIKELY( !src_bytes_in_cur_region ) ) {
        /* Go to next one */
        if( FD_UNLIKELY( ++src_region_idx>=vm->input_mem_regions_cnt ) ) {
          *_ret = 1;
          return FD_VM_ERR_ABORT;
        }
        src_haddr = (uchar*)vm->input_mem_regions[ src_region_idx ].haddr;
        src_idx = 0UL;
      }

      dst_haddr[ dst_idx ] = src_haddr[ src_idx ];

      dst_bytes_in_cur_region--;
      src_bytes_in_cur_region--;
      dst_idx++;
      src_idx++;
    }
  }

  *_ret = 0;
  return FD_VM_SUCCESS;
}
