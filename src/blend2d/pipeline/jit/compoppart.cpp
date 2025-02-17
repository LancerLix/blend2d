// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include "../../api-build_p.h"
#if BL_TARGET_ARCH_X86 && !defined(BL_BUILD_NO_JIT)

#include "../../runtime_p.h"
#include "../../pipeline/jit/compoppart_p.h"
#include "../../pipeline/jit/fetchpart_p.h"
#include "../../pipeline/jit/fetchpatternpart_p.h"
#include "../../pipeline/jit/fetchpixelptrpart_p.h"
#include "../../pipeline/jit/fetchsolidpart_p.h"
#include "../../pipeline/jit/pipecompiler_p.h"

namespace BLPipeline {
namespace JIT {

// BLPipeline::JIT::CompOpPart - Construction & Destruction
// ========================================================

CompOpPart::CompOpPart(PipeCompiler* pc, uint32_t compOp, FetchPart* dstPart, FetchPart* srcPart) noexcept
  : PipePart(pc, PipePartType::kComposite),
    _compOp(compOp),
    _pixelType(dstPart->hasRGB() ? PixelType::kRGBA32 : PixelType::kA8),
    _isInPartialMode(false),
    _hasDa(dstPart->hasAlpha()),
    _hasSa(srcPart->hasAlpha()),
    _solidPre("solid", _pixelType),
    _partialPixel("partial", _pixelType) {

  _mask->reset();

  // Initialize the children of this part.
  _children[kIndexDstPart] = dstPart;
  _children[kIndexSrcPart] = srcPart;
  _childCount = 2;

  SimdWidth maxSimdWidth = SimdWidth::k128;
  switch (pixelType()) {
    case PixelType::kA8: {
      maxSimdWidth = SimdWidth::k512;
      break;
    }

    case PixelType::kRGBA32: {
      switch (compOp) {
        case BL_COMP_OP_SRC_OVER    :
        case BL_COMP_OP_SRC_COPY    :
        case BL_COMP_OP_SRC_IN      :
        case BL_COMP_OP_SRC_OUT     :
        case BL_COMP_OP_SRC_ATOP    :
        case BL_COMP_OP_DST_OVER    :
        case BL_COMP_OP_DST_IN      :
        case BL_COMP_OP_DST_OUT     :
        case BL_COMP_OP_DST_ATOP    :
        case BL_COMP_OP_XOR         :
        case BL_COMP_OP_CLEAR       :
        case BL_COMP_OP_PLUS        :
        case BL_COMP_OP_MINUS       :
        case BL_COMP_OP_MODULATE    :
        case BL_COMP_OP_MULTIPLY    :
        case BL_COMP_OP_SCREEN      :
        case BL_COMP_OP_OVERLAY     :
        case BL_COMP_OP_DARKEN      :
        case BL_COMP_OP_LIGHTEN     :
        case BL_COMP_OP_LINEAR_BURN :
        case BL_COMP_OP_PIN_LIGHT   :
        case BL_COMP_OP_HARD_LIGHT  :
        case BL_COMP_OP_DIFFERENCE  :
        case BL_COMP_OP_EXCLUSION   :
          maxSimdWidth = SimdWidth::k512;
          break;

        case BL_COMP_OP_COLOR_DODGE :
        case BL_COMP_OP_COLOR_BURN  :
        case BL_COMP_OP_LINEAR_LIGHT:
        case BL_COMP_OP_SOFT_LIGHT  :
          break;
      }
      break;
    }

    default:
      BL_NOT_REACHED();
  }

  _maxSimdWidthSupported = maxSimdWidth;
}

// BLPipeline::JIT::CompOpPart - Prepare
// =====================================

void CompOpPart::preparePart() noexcept {
  bool isSolid = srcPart()->isSolid();
  uint32_t maxPixels = 0;
  uint32_t pixelLimit = 64;

  _partFlags |= (dstPart()->partFlags() | srcPart()->partFlags()) & PipePartFlags::kFetchFlags;

  if (srcPart()->hasMaskedAccess() && dstPart()->hasMaskedAccess())
    _partFlags |= PipePartFlags::kMaskedAccess;

  // Limit the maximum pixel-step to 4 it the style is not solid and the target is not 64-bit.
  // There's not enough registers to process 8 pixels in parallel in 32-bit mode.
  if (blRuntimeIs32Bit() && !isSolid && _pixelType != PixelType::kA8)
    pixelLimit = 4;

  // Decrease the maximum pixels to 4 if the source is complex to fetch. In such case fetching and processing more
  // pixels would result in emitting bloated pipelines that are not faster compared to pipelines working with just
  // 4 pixels at a time.
  if (dstPart()->isComplexFetch() || srcPart()->isComplexFetch())
    pixelLimit = 4;

  switch (pixelType()) {
    case PixelType::kA8: {
      maxPixels = 8;
      break;
    }

    case PixelType::kRGBA32: {
      switch (compOp()) {
        case BL_COMP_OP_SRC_OVER    : maxPixels = 8; break;
        case BL_COMP_OP_SRC_COPY    : maxPixels = 8; break;
        case BL_COMP_OP_SRC_IN      : maxPixels = 8; break;
        case BL_COMP_OP_SRC_OUT     : maxPixels = 8; break;
        case BL_COMP_OP_SRC_ATOP    : maxPixels = 8; break;
        case BL_COMP_OP_DST_OVER    : maxPixels = 8; break;
        case BL_COMP_OP_DST_IN      : maxPixels = 8; break;
        case BL_COMP_OP_DST_OUT     : maxPixels = 8; break;
        case BL_COMP_OP_DST_ATOP    : maxPixels = 8; break;
        case BL_COMP_OP_XOR         : maxPixels = 8; break;
        case BL_COMP_OP_CLEAR       : maxPixels = 8; break;
        case BL_COMP_OP_PLUS        : maxPixels = 8; break;
        case BL_COMP_OP_MINUS       : maxPixels = 4; break;
        case BL_COMP_OP_MODULATE    : maxPixels = 8; break;
        case BL_COMP_OP_MULTIPLY    : maxPixels = 8; break;
        case BL_COMP_OP_SCREEN      : maxPixels = 8; break;
        case BL_COMP_OP_OVERLAY     : maxPixels = 4; break;
        case BL_COMP_OP_DARKEN      : maxPixels = 8; break;
        case BL_COMP_OP_LIGHTEN     : maxPixels = 8; break;
        case BL_COMP_OP_COLOR_DODGE : maxPixels = 1; break;
        case BL_COMP_OP_COLOR_BURN  : maxPixels = 1; break;
        case BL_COMP_OP_LINEAR_BURN : maxPixels = 8; break;
        case BL_COMP_OP_LINEAR_LIGHT: maxPixels = 1; break;
        case BL_COMP_OP_PIN_LIGHT   : maxPixels = 4; break;
        case BL_COMP_OP_HARD_LIGHT  : maxPixels = 4; break;
        case BL_COMP_OP_SOFT_LIGHT  : maxPixels = 1; break;
        case BL_COMP_OP_DIFFERENCE  : maxPixels = 4; break;
        case BL_COMP_OP_EXCLUSION   : maxPixels = 4; break;

        default:
          BL_NOT_REACHED();
      }
      break;
    }

    default:
      BL_NOT_REACHED();
  }

  if (maxPixels > 1) {
    maxPixels *= pc->simdMultiplier();
    pixelLimit *= pc->simdMultiplier();
  }

  // Descrease to N pixels at a time if the fetch part doesn't support more.
  // This is suboptimal, but can happen if the fetch part is not optimized.
  maxPixels = blMin(maxPixels, pixelLimit, srcPart()->maxPixels());

  if (isRGBA32Pixel()) {
    if (maxPixels >= 4)
      _minAlignment = 16;
  }

  setMaxPixels(maxPixels);
}

// BLPipeline::JIT::CompOpPart - Init & Fini
// =========================================

void CompOpPart::init(x86::Gp& x, x86::Gp& y, uint32_t pixelGranularity) noexcept {
  _pixelGranularity = uint8_t(pixelGranularity);

  dstPart()->init(x, y, pixelType(), pixelGranularity);
  srcPart()->init(x, y, pixelType(), pixelGranularity);
}

void CompOpPart::fini() noexcept {
  dstPart()->fini();
  srcPart()->fini();

  _pixelGranularity = 0;
}

// BLPipeline::JIT::CompOpPart - Optimization Opportunities
// ========================================================

bool CompOpPart::shouldOptimizeOpaqueFill() const noexcept {
  // Should be always optimized if the source is not solid.
  if (!srcPart()->isSolid())
    return true;

  // Do not optimize if the CompOp is TypeA. This operator doesn't need any
  // special handling as the source pixel is multiplied with mask before it's
  // passed to the compositor.
  if (blTestFlag(compOpFlags(), BLCompOpFlags::kTypeA))
    return false;

  // Modulate operator just needs to multiply source with mask and add (1 - m)
  // to it.
  if (isModulate())
    return false;

  // We assume that in all other cases there is a benefit of using optimized
  // `cMask` loop for a fully opaque mask.
  return true;
}

bool CompOpPart::shouldJustCopyOpaqueFill() const noexcept {
  if (!isSrcCopy())
    return false;

  if (srcPart()->isSolid())
    return true;

  if (srcPart()->isFetchType(FetchType::kPatternAlignedBlit) && srcPart()->format() == dstPart()->format())
    return true;

  return false;
}

// BLPipeline::JIT::CompOpPart - Advance
// =====================================

void CompOpPart::startAtX(const x86::Gp& x) noexcept {
  dstPart()->startAtX(x);
  srcPart()->startAtX(x);
}

void CompOpPart::advanceX(const x86::Gp& x, const x86::Gp& diff) noexcept {
  dstPart()->advanceX(x, diff);
  srcPart()->advanceX(x, diff);
}

void CompOpPart::advanceY() noexcept {
  dstPart()->advanceY();
  srcPart()->advanceY();
}

// BLPipeline::JIT::CompOpPart - Prefetch & Postfetch
// ==================================================

void CompOpPart::prefetch1() noexcept {
  dstPart()->prefetch1();
  srcPart()->prefetch1();
}

void CompOpPart::enterN() noexcept {
  dstPart()->enterN();
  srcPart()->enterN();
}

void CompOpPart::leaveN() noexcept {
  dstPart()->leaveN();
  srcPart()->leaveN();
}

void CompOpPart::prefetchN() noexcept {
  dstPart()->prefetchN();
  srcPart()->prefetchN();
}

void CompOpPart::postfetchN() noexcept {
  dstPart()->postfetchN();
  srcPart()->postfetchN();
}

// BLPipeline::JIT::CompOpPart - Fetch
// ===================================

void CompOpPart::dstFetch(Pixel& p, PixelCount n, PixelFlags flags, PixelPredicate& predicate) noexcept {
  dstPart()->fetch(p, n, flags, predicate);
}

void CompOpPart::srcFetch(Pixel& p, PixelCount n, PixelFlags flags, PixelPredicate& predicate) noexcept {
  // Pixels must match as we have already preconfigured the CompOpPart.
  BL_ASSERT(p.type() == pixelType());

  if (p.count() == 0)
    p.setCount(n);

  // Composition with a preprocessed solid color.
  if (isUsingSolidPre()) {
    Pixel& s = _solidPre;

    // INJECT:
    {
      ScopedInjector injector(cc, &_cMaskLoopHook);
      pc->x_satisfy_solid(s, flags);
    }

    if (p.isRGBA32()) {
      SimdWidth pcSimdWidth = pc->simdWidthOf(DataWidth::k32, n);
      SimdWidth ucSimdWidth = pc->simdWidthOf(DataWidth::k64, n);

      uint32_t pcCount = pc->regCountOf(DataWidth::k32, n);
      uint32_t ucCount = pc->regCountOf(DataWidth::k64, n);

      if (blTestFlag(flags, PixelFlags::kImmutable)) {
        if (blTestFlag(flags, PixelFlags::kPC)) {
          p.pc.init(SimdWidthUtils::cloneVecAs(s.pc[0], pcSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUC)) {
          p.uc.init(SimdWidthUtils::cloneVecAs(s.uc[0], ucSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUA)) {
          p.ua.init(SimdWidthUtils::cloneVecAs(s.ua[0], ucSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUI)) {
          p.ui.init(SimdWidthUtils::cloneVecAs(s.ui[0], ucSimdWidth));
        }
      }
      else {
        if (blTestFlag(flags, PixelFlags::kPC)) {
          pc->newVecArray(p.pc, pcCount, pcSimdWidth, p.name(), "pc");
          pc->v_mov(p.pc, SimdWidthUtils::cloneVecAs(s.pc[0], pcSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUC)) {
          pc->newVecArray(p.uc, ucCount, ucSimdWidth, p.name(), "uc");
          pc->v_mov(p.uc, SimdWidthUtils::cloneVecAs(s.uc[0], ucSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUA)) {
          pc->newVecArray(p.ua, ucCount, ucSimdWidth, p.name(), "ua");
          pc->v_mov(p.ua, SimdWidthUtils::cloneVecAs(s.ua[0], ucSimdWidth));
        }

        if (blTestFlag(flags, PixelFlags::kUI)) {
          pc->newVecArray(p.ui, ucCount, ucSimdWidth, p.name(), "ui");
          pc->v_mov(p.ui, SimdWidthUtils::cloneVecAs(s.ui[0], ucSimdWidth));
        }
      }
    }
    else if (p.isA8()) {
      // TODO: A8 pipepine.
      BL_ASSERT(false);
    }

    return;
  }

  // Partial mode is designed to fetch pixels on the right side of the border one by one, so it's an error
  // if the pipeline requests more than 1 pixel at a time.
  if (isInPartialMode()) {
    BL_ASSERT(n == 1);

    if (p.isRGBA32()) {
      if (!blTestFlag(flags, PixelFlags::kImmutable)) {
        if (blTestFlag(flags, PixelFlags::kUC)) {
          pc->newXmmArray(p.uc, 1, "uc");
          pc->v_mov_u8_u16(p.uc[0], _partialPixel.pc[0].xmm());
        }
        else {
          pc->newXmmArray(p.pc, 1, "pc");
          pc->v_mov(p.pc[0], _partialPixel.pc[0].xmm());
        }
      }
      else {
        p.pc.init(_partialPixel.pc[0]);
      }
    }
    else if (p.isA8()) {
      p.sa = cc->newUInt32("sa");
      pc->v_extract_u16(p.sa, _partialPixel.ua[0].xmm(), 0);
    }

    pc->x_satisfy_pixel(p, flags);
    return;
  }

  srcPart()->fetch(p, n, flags, predicate);
}

// BLPipeline::JIT::CompOpPart - PartialFetch
// ==========================================

void CompOpPart::enterPartialMode(PixelFlags partialFlags) noexcept {
  // Doesn't apply to solid fills.
  if (isUsingSolidPre())
    return;

  // TODO: [PIPEGEN] We only support partial fetch of 4 pixels at the moment.
  BL_ASSERT(!isInPartialMode());
  BL_ASSERT(pixelGranularity() == 4);

  switch (pixelType()) {
    case PixelType::kA8: {
      srcFetch(_partialPixel, pixelGranularity(), PixelFlags::kUA | partialFlags, pc->emptyPredicate());
      break;
    }

    case PixelType::kRGBA32: {
      srcFetch(_partialPixel, pixelGranularity(), PixelFlags::kPC | partialFlags, pc->emptyPredicate());
      break;
    }
  }

  _isInPartialMode = true;
}

void CompOpPart::exitPartialMode() noexcept {
  // Doesn't apply to solid fills.
  if (isUsingSolidPre())
    return;

  BL_ASSERT(isInPartialMode());

  _isInPartialMode = false;
  _partialPixel.resetAllExceptTypeAndName();
}

void CompOpPart::nextPartialPixel() noexcept {
  if (!isInPartialMode())
    return;

  switch (pixelType()) {
    case PixelType::kA8: {
      const x86::Vec& pix = _partialPixel.ua[0];
      pc->v_srlb_u128(pix, pix, 2);
      break;
    }

    case PixelType::kRGBA32: {
      const x86::Vec& pix = _partialPixel.pc[0];
      pc->v_srlb_u128(pix, pix, 4);
      break;
    }
  }
}

// BLPipeline::JIT::CompOpPart - CMask - Init & Fini
// =================================================

void CompOpPart::cMaskInit(const x86::Mem& mem) noexcept {
  switch (pixelType()) {
    case PixelType::kA8: {
      x86::Gp mGp = cc->newUInt32("msk");
      pc->i_load_u8(mGp, mem);
      cMaskInitA8(mGp, x86::Vec());
      break;
    }

    case PixelType::kRGBA32: {
      x86::Vec vm = pc->newVec("vm");
      pc->v_broadcast_u16(vm, mem);
      cMaskInitRGBA32(vm);
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::cMaskInit(const x86::Gp& sm_, const x86::Vec& vm_) noexcept {
  x86::Gp sm(sm_);
  x86::Vec vm(vm_);

  switch (pixelType()) {
    case PixelType::kA8: {
      cMaskInitA8(sm, vm);
      break;
    }

    case PixelType::kRGBA32: {
      if (!vm.isValid() && sm.isValid()) {
        vm = pc->newVec("vm");
        pc->v_broadcast_u16(vm, sm);
      }

      cMaskInitRGBA32(vm);
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::cMaskInitOpaque() noexcept {
  switch (pixelType()) {
    case PixelType::kA8: {
      cMaskInitA8(x86::Gp(), x86::Vec());
      break;
    }

    case PixelType::kRGBA32: {
      cMaskInitRGBA32(x86::Vec());
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::cMaskFini() noexcept {
  switch (pixelType()) {
    case PixelType::kA8: {
      cMaskFiniA8();
      break;
    }

    case PixelType::kRGBA32: {
      cMaskFiniRGBA32();
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::_cMaskLoopInit(CMaskLoopType loopType) noexcept {
  // Make sure `_cMaskLoopInit()` and `_cMaskLoopFini()` are used as a pair.
  BL_ASSERT(_cMaskLoopType == CMaskLoopType::kNone);
  BL_ASSERT(_cMaskLoopHook == nullptr);

  _cMaskLoopType = loopType;
  _cMaskLoopHook = cc->cursor();
}

void CompOpPart::_cMaskLoopFini() noexcept {
  // Make sure `_cMaskLoopInit()` and `_cMaskLoopFini()` are used as a pair.
  BL_ASSERT(_cMaskLoopType != CMaskLoopType::kNone);
  BL_ASSERT(_cMaskLoopHook != nullptr);

  _cMaskLoopType = CMaskLoopType::kNone;
  _cMaskLoopHook = nullptr;
}

// BLPipeline::JIT::CompOpPart - CMask - Generic Loop
// ==================================================

void CompOpPart::cMaskGenericLoop(x86::Gp& i) noexcept {
  if (isLoopOpaque() && shouldJustCopyOpaqueFill()) {
    cMaskMemcpyOrMemsetLoop(i);
    return;
  }

  cMaskGenericLoopVec(i);
}

void CompOpPart::cMaskGenericLoopVec(x86::Gp& i) noexcept {
  x86::Gp dPtr = dstPart()->as<FetchPixelPtrPart>()->ptr();

  // 1 pixel at a time.
  if (maxPixels() == 1) {
    Label L_Loop = cc->newLabel();

    prefetch1();

    cc->bind(L_Loop);
    cMaskProcStoreAdvance(dPtr, PixelCount(1), Alignment(1));
    cc->sub(i, 1);
    cc->jnz(L_Loop);

    return;
  }

  BL_ASSERT(minAlignment() >= 1);
  uint32_t alignmentMask = minAlignment().value() - 1;

  // 4+ pixels at a time [no alignment].
  if (maxPixels() == 4 && minAlignment() == 1) {
    Label L_Loop1 = cc->newLabel();
    Label L_Loop4 = cc->newLabel();
    Label L_Skip4 = cc->newLabel();
    Label L_Exit = cc->newLabel();

    cc->sub(i, 4);
    cc->jc(L_Skip4);

    enterN();
    prefetchN();

    cc->bind(L_Loop4);
    cMaskProcStoreAdvance(dPtr, PixelCount(4));
    cc->sub(i, 4);
    cc->jnc(L_Loop4);

    postfetchN();
    leaveN();

    cc->bind(L_Skip4);
    prefetch1();
    cc->add(i, 4);
    cc->jz(L_Exit);

    cc->bind(L_Loop1);
    cMaskProcStoreAdvance(dPtr, PixelCount(1));
    cc->sub(i, 1);
    cc->jnz(L_Loop1);

    cc->bind(L_Exit);
    return;
  }

  // 4+ pixels at a time [with alignment].
  if (maxPixels() == 4 && minAlignment() != 1) {
    Label L_Loop1     = cc->newLabel();
    Label L_Loop4     = cc->newLabel();
    Label L_Aligned   = cc->newLabel();
    Label L_Unaligned = cc->newLabel();
    Label L_Exit      = cc->newLabel();

    pc->i_test(dPtr, alignmentMask);
    cc->jnz(L_Unaligned);

    cc->cmp(i, 4);
    cc->jae(L_Aligned);

    cc->bind(L_Unaligned);
    prefetch1();

    cc->bind(L_Loop1);
    cMaskProcStoreAdvance(dPtr, PixelCount(1));
    cc->sub(i, 1);
    cc->jz(L_Exit);

    pc->i_test(dPtr, alignmentMask);
    cc->jnz(L_Loop1);

    cc->cmp(i, 4);
    cc->jb(L_Loop1);

    cc->bind(L_Aligned);
    cc->sub(i, 4);
    dstPart()->as<FetchPixelPtrPart>()->setAlignment(Alignment(16));

    enterN();
    prefetchN();

    cc->bind(L_Loop4);
    cMaskProcStoreAdvance(dPtr, PixelCount(4), Alignment(16));
    cc->sub(i, 4);
    cc->jnc(L_Loop4);

    postfetchN();
    leaveN();
    dstPart()->as<FetchPixelPtrPart>()->setAlignment(Alignment(0));

    prefetch1();

    cc->add(i, 4);
    cc->jnz(L_Loop1);

    cc->bind(L_Exit);
    return;
  }

  // 8+ pixels at a time [no alignment].
  if (maxPixels() == 8 && minAlignment() == 1) {
    Label L_Loop1 = cc->newLabel();
    Label L_Loop4 = cc->newLabel();
    Label L_Loop8 = cc->newLabel();
    Label L_Skip4 = cc->newLabel();
    Label L_Skip8 = cc->newLabel();
    Label L_Init1 = cc->newLabel();
    Label L_Exit = cc->newLabel();

    cc->sub(i, 4);
    cc->jc(L_Skip4);

    enterN();
    prefetchN();

    cc->sub(i, 4);
    cc->jc(L_Skip8);

    cc->bind(L_Loop8);
    cMaskProcStoreAdvance(dPtr, PixelCount(8));
    cc->sub(i, 8);
    cc->jnc(L_Loop8);

    cc->bind(L_Skip8);
    cc->add(i, 4);
    cc->jnc(L_Init1);

    cc->bind(L_Loop4);
    cMaskProcStoreAdvance(dPtr, PixelCount(4));
    cc->sub(i, 4);
    cc->jnc(L_Loop4);

    cc->bind(L_Init1);
    postfetchN();
    leaveN();

    cc->bind(L_Skip4);
    prefetch1();
    cc->add(i, 4);
    cc->jz(L_Exit);

    cc->bind(L_Loop1);
    cMaskProcStoreAdvance(dPtr, PixelCount(1));
    cc->sub(i, 1);
    cc->jnz(L_Loop1);

    cc->bind(L_Exit);
    return;
  }

  // 8+ pixels at a time [with alignment].
  if (maxPixels() == 8 && minAlignment() != 1) {
    Label L_Loop1   = cc->newLabel();
    Label L_Loop8   = cc->newLabel();
    Label L_Skip8   = cc->newLabel();
    Label L_Skip4   = cc->newLabel();
    Label L_Aligned = cc->newLabel();
    Label L_Exit    = cc->newLabel();

    cc->test(dPtr.r8(), alignmentMask);
    cc->jz(L_Aligned);

    prefetch1();

    cc->bind(L_Loop1);
    cMaskProcStoreAdvance(dPtr, PixelCount(1));
    cc->sub(i, 1);
    cc->jz(L_Exit);

    cc->test(dPtr.r8(), alignmentMask);
    cc->jnz(L_Loop1);

    cc->bind(L_Aligned);
    cc->cmp(i, 4);
    cc->jb(L_Loop1);

    dstPart()->as<FetchPixelPtrPart>()->setAlignment(Alignment(16));
    enterN();
    prefetchN();

    cc->sub(i, 8);
    cc->jc(L_Skip8);

    cc->bind(L_Loop8);
    cMaskProcStoreAdvance(dPtr, PixelCount(8), minAlignment());
    cc->sub(i, 8);
    cc->jnc(L_Loop8);

    cc->bind(L_Skip8);
    cc->add(i, 4);
    cc->jnc(L_Skip4);

    cMaskProcStoreAdvance(dPtr, PixelCount(4), minAlignment());
    cc->sub(i, 4);
    cc->bind(L_Skip4);

    postfetchN();
    leaveN();
    dstPart()->as<FetchPixelPtrPart>()->setAlignment(Alignment(0));

    prefetch1();

    cc->add(i, 4);
    cc->jnz(L_Loop1);

    cc->bind(L_Exit);
    return;
  }

  // 16 pixels at a time.
  if (maxPixels() == 16) {
    Label L_Loop16 = cc->newLabel();
    Label L_Skip16 = cc->newLabel();
    Label L_Exit = cc->newLabel();

    enterN();
    prefetchN();

    cc->sub(i, 16);
    cc->jc(L_Skip16);

    cc->bind(L_Loop16);
    cMaskProcStoreAdvance(dPtr, PixelCount(16), Alignment(1));
    cc->sub(i, 16);
    cc->jnc(L_Loop16);

    cc->bind(L_Skip16);
    cc->add(i, 16);
    cc->jz(L_Exit);

    if (pc->use512BitSimd()) {
      if (hasMaskedAccess()) {
        PixelPredicate predicate(16u, PredicateFlags::kNeverEmptyOrFull, i);
        cMaskProcStoreAdvance(dPtr, PixelCount(16), Alignment(1), predicate);
      }
      else {
        // TODO: YMM/ZMM pipeline.
        BL_ASSERT(0);
      }
    }
    else {
      Label L_Skip8 = cc->newLabel();
      cc->cmp(i, 8);
      cc->jc(L_Skip8);

      cMaskProcStoreAdvance(dPtr, PixelCount(8), Alignment(1));
      cc->sub(i, 8);
      cc->jz(L_Exit);

      cc->bind(L_Skip8);
      if (hasMaskedAccess()) {
        PixelPredicate predicate(8u, PredicateFlags::kNeverEmptyOrFull, i);
        cMaskProcStoreAdvance(dPtr, PixelCount(8), Alignment(1), predicate);
      }
      else {
        // TODO: YMM pipeline.
        BL_ASSERT(0);
      }
    }

    cc->bind(L_Exit);

    postfetchN();
    leaveN();

    return;
  }

  // 32 pixels at a time.
  if (maxPixels() == 32) {
    Label L_Loop32 = cc->newLabel();
    Label L_Skip32 = cc->newLabel();
    Label L_Loop8 = cc->newLabel();
    Label L_Skip8 = cc->newLabel();
    Label L_Exit = cc->newLabel();

    enterN();
    prefetchN();

    cc->sub(i, 32);
    cc->jc(L_Skip32);

    cc->bind(L_Loop32);
    cMaskProcStoreAdvance(dPtr, PixelCount(32), Alignment(1));
    cc->sub(i, 32);
    cc->jnc(L_Loop32);

    cc->bind(L_Skip32);
    cc->add(i, 32);
    cc->jz(L_Exit);

    cc->sub(i, 8);
    cc->jc(L_Skip8);

    cc->bind(L_Loop8);
    cMaskProcStoreAdvance(dPtr, PixelCount(8), Alignment(1));
    cc->sub(i, 8);
    cc->jnc(L_Loop8);

    cc->bind(L_Skip8);
    cc->add(i, 8);
    cc->jz(L_Exit);

    if (hasMaskedAccess()) {
      PixelPredicate predicate(8u, PredicateFlags::kNeverEmptyOrFull, i);
      cMaskProcStoreAdvance(dPtr, PixelCount(8), Alignment(1), predicate);
    }
    else {
      // TODO: YMM pipeline.
      BL_ASSERT(0);
    }

    cc->bind(L_Exit);

    postfetchN();
    leaveN();

    return;
  }

  BL_NOT_REACHED();
}

// BLPipeline::JIT::CompOpPart - CMask - Granular Loop
// ===================================================

void CompOpPart::cMaskGranularLoop(x86::Gp& i) noexcept {
  if (isLoopOpaque() && shouldJustCopyOpaqueFill()) {
    cMaskMemcpyOrMemsetLoop(i);
    return;
  }

  cMaskGranularLoopVec(i);
}

void CompOpPart::cMaskGranularLoopVec(x86::Gp& i) noexcept {
  BL_ASSERT(pixelGranularity() == 4);

  x86::Gp dPtr = dstPart()->as<FetchPixelPtrPart>()->ptr();
  if (pixelGranularity() == 4) {
    // 1 pixel at a time.
    if (maxPixels() == 1) {
      Label L_Loop = cc->newLabel();
      Label L_Step = cc->newLabel();

      cc->bind(L_Loop);
      enterPartialMode();

      cc->bind(L_Step);
      cMaskProcStoreAdvance(dPtr, PixelCount(1));
      cc->sub(i, 1);
      nextPartialPixel();

      cc->test(i, 0x3);
      cc->jnz(L_Step);

      exitPartialMode();

      cc->test(i, i);
      cc->jnz(L_Loop);

      return;
    }

    // 4+ pixels at a time.
    if (maxPixels() == 4) {
      Label L_Loop = cc->newLabel();

      cc->bind(L_Loop);
      cMaskProcStoreAdvance(dPtr, PixelCount(4));
      cc->sub(i, 4);
      cc->jnz(L_Loop);

      return;
    }

    // 8+ pixels at a time.
    if (maxPixels() == 8) {
      Label L_Loop_Iter8 = cc->newLabel();
      Label L_Skip = cc->newLabel();
      Label L_End = cc->newLabel();

      cc->sub(i, 8);
      cc->jc(L_Skip);

      cc->bind(L_Loop_Iter8);
      cMaskProcStoreAdvance(dPtr, PixelCount(8));
      cc->sub(i, 8);
      cc->jnc(L_Loop_Iter8);

      cc->bind(L_Skip);
      cc->add(i, 8);
      cc->jz(L_End);

      // 4 remaining pixels.
      cMaskProcStoreAdvance(dPtr, PixelCount(4));

      cc->bind(L_End);
      return;
    }

    // 16 pixels at a time.
    if (maxPixels() == 16) {
      Label L_Loop_Iter16 = cc->newLabel();
      Label L_Loop_Iter4 = cc->newLabel();
      Label L_Skip = cc->newLabel();
      Label L_End = cc->newLabel();

      cc->sub(i, 16);
      cc->jc(L_Skip);

      cc->bind(L_Loop_Iter16);
      cMaskProcStoreAdvance(dPtr, PixelCount(16));
      cc->sub(i, 16);
      cc->jnc(L_Loop_Iter16);

      cc->bind(L_Skip);
      cc->add(i, 16);
      cc->jz(L_End);

      // 4 remaining pixels.
      cc->bind(L_Loop_Iter4);
      cMaskProcStoreAdvance(dPtr, PixelCount(4));
      cc->sub(i, 4);
      cc->jnz(L_Loop_Iter4);

      cc->bind(L_End);
      return;
    }
  }

  BL_NOT_REACHED();
}

// BLPipeline::JIT::CompOpPart - CMask - MemCpy & MemSet Loop
// ==========================================================

void CompOpPart::cMaskMemcpyOrMemsetLoop(x86::Gp& i) noexcept {
  BL_ASSERT(shouldJustCopyOpaqueFill());
  x86::Gp dPtr = dstPart()->as<FetchPixelPtrPart>()->ptr();

  if (srcPart()->isSolid()) {
    // Optimized solid opaque fill -> MemSet.
    BL_ASSERT(_solidOpt.px.isValid());
    pc->x_inline_pixel_fill_loop(dPtr, _solidOpt.px, i, 64, dstPart()->bpp(), pixelGranularity().value());
  }
  else if (srcPart()->isFetchType(FetchType::kPatternAlignedBlit)) {
    // Optimized solid opaque blit -> MemCopy.
    pc->x_inline_pixel_copy_loop(dPtr, srcPart()->as<FetchSimplePatternPart>()->f->srcp1, i, 64, dstPart()->bpp(), pixelGranularity().value(), dstPart()->format());
  }
  else {
    BL_NOT_REACHED();
  }
}

// BLPipeline::JIT::CompOpPart - CMask - Composition Helpers
// =========================================================

void CompOpPart::cMaskProcStoreAdvance(const x86::Gp& dPtr, PixelCount n, Alignment alignment) noexcept {
  PixelPredicate ptrMask;
  cMaskProcStoreAdvance(dPtr, n, alignment, ptrMask);
}

void CompOpPart::cMaskProcStoreAdvance(const x86::Gp& dPtr, PixelCount n, Alignment alignment, PixelPredicate& predicate) noexcept {
  Pixel dPix("d", pixelType());

  switch (pixelType()) {
    case PixelType::kA8: {
      if (n == 1)
        cMaskProcA8Gp(dPix, PixelFlags::kSA | PixelFlags::kImmutable);
      else
        cMaskProcA8Vec(dPix, n, PixelFlags::kPA | PixelFlags::kImmutable, predicate);
      pc->x_store_pixel_advance(dPtr, dPix, n, 1, alignment, predicate);
      break;
    }

    case PixelType::kRGBA32: {
      cMaskProcRGBA32Vec(dPix, n, PixelFlags::kImmutable, predicate);
      pc->x_store_pixel_advance(dPtr, dPix, n, 4, alignment, predicate);
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

// BLPipeline::JIT::CompOpPart - VMask - Composition Helpers
// =========================================================

void CompOpPart::vMaskGenericLoop(x86::Gp& i, const x86::Gp& dPtr, const x86::Gp& mPtr, GlobalAlpha& ga, const Label& done) noexcept {
  Label L_Done = done.isValid() ? done : cc->newLabel();

  if (maxPixels() >= 4) {
    Label L_Loop4_Iter = cc->newLabel();
    Label L_Loop4_Skip = cc->newLabel();

    cc->sub(i, 4);
    cc->jc(L_Loop4_Skip);

    enterN();
    prefetchN();

    if (maxPixels() >= 8) {
      Label L_Loop8_Iter = cc->newLabel();
      Label L_Loop8_Skip = cc->newLabel();

      cc->sub(i, 4);
      cc->jc(L_Loop8_Skip);

      cc->bind(L_Loop8_Iter);
      vMaskGenericStep(dPtr, PixelCount(8), mPtr, ga.vm());
      cc->sub(i, 8);
      cc->jnc(L_Loop8_Iter);

      cc->bind(L_Loop8_Skip);
      cc->add(i, 4);
      cc->js(L_Loop4_Skip);
    }

    cc->bind(L_Loop4_Iter);
    vMaskGenericStep(dPtr, PixelCount(4), mPtr, ga.vm());
    cc->sub(i, 4);
    cc->jnc(L_Loop4_Iter);

    postfetchN();
    leaveN();

    cc->bind(L_Loop4_Skip);
    prefetch1();
    cc->add(i, 4);
    cc->jz(L_Done);
  }

  Label L_Loop1_Iter = cc->newLabel();
  x86::Reg gaSinglePixel;
  if (ga.isInitialized())
    gaSinglePixel = pixelType() == PixelType::kA8 ? ga.sm().as<x86::Reg>() : ga.vm().as<x86::Reg>();

  cc->bind(L_Loop1_Iter);
  vMaskGenericStep(dPtr, PixelCount(1), mPtr, gaSinglePixel);
  cc->sub(i, 1);
  cc->jnz(L_Loop1_Iter);

  if (done.isValid())
    cc->jmp(L_Done);
  else
    cc->bind(L_Done);
}

void CompOpPart::vMaskGenericStep(const x86::Gp& dPtr, PixelCount n, const x86::Gp& mPtr, const x86::Reg& ga) noexcept {
  switch (pixelType()) {
    case PixelType::kA8: {
      if (n == 1u) {
        x86::Gp sm = cc->newUInt32("sm");

        pc->i_load_u8(sm, x86::ptr(mPtr));
        pc->i_add(mPtr, mPtr, n.value());

        if (ga.isValid()) {
          BL_ASSERT(ga.isGp());

          pc->i_mul(sm, sm, ga.as<x86::Gp>().r32());
          pc->i_div_255_u32(sm, sm);
        }

        Pixel dPix("d", pixelType());
        vMaskProcA8Gp(dPix, PixelFlags::kSA | PixelFlags::kImmutable, sm, false);
        pc->x_store_pixel_advance(dPtr, dPix, n, 1, Alignment(1), pc->emptyPredicate());
      }
      else {
        // Global alpha must be either invalid or a vector register, to apply it. It cannot be scalar.
        BL_ASSERT(!ga.isValid() || ga.isVec());

        VecArray vm;
        pc->x_fetch_mask_a8_advance(vm, n, pixelType(), mPtr, ga.as<x86::Vec>());
        vMaskProcStoreAdvance(dPtr, n, vm, false);
      }
      break;
    }

    case PixelType::kRGBA32: {
      // Global alpha must be either invalid or a vector register, to apply it. It cannot be scalar.
      BL_ASSERT(!ga.isValid() || ga.isVec());

      VecArray vm;
      pc->x_fetch_mask_a8_advance(vm, n, pixelType(), mPtr, ga.as<x86::Vec>());
      vMaskProcStoreAdvance(dPtr, n, vm, false);
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::vMaskProcStoreAdvance(const x86::Gp& dPtr, PixelCount n, VecArray& vm, bool vmImmutable, Alignment alignment) noexcept {
  PixelPredicate ptrMask;
  vMaskProcStoreAdvance(dPtr, n, vm, vmImmutable, alignment, ptrMask);
}

void CompOpPart::vMaskProcStoreAdvance(const x86::Gp& dPtr, PixelCount n, VecArray& vm, bool vmImmutable, Alignment alignment, PixelPredicate& predicate) noexcept {
  Pixel dPix("d", pixelType());

  switch (pixelType()) {
    case PixelType::kA8: {
      BL_ASSERT(n != 1);

      vMaskProcA8Vec(dPix, n, PixelFlags::kPA | PixelFlags::kImmutable, vm, vmImmutable, predicate);
      pc->x_store_pixel_advance(dPtr, dPix, n, 1, alignment, predicate);
      break;
    }

    case PixelType::kRGBA32: {
      vMaskProcRGBA32Vec(dPix, n, PixelFlags::kImmutable, vm, vmImmutable, predicate);
      pc->x_store_pixel_advance(dPtr, dPix, n, 4, alignment, predicate);
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

void CompOpPart::vMaskProc(Pixel& out, PixelFlags flags, x86::Gp& msk, bool mImmutable) noexcept {
  switch (pixelType()) {
    case PixelType::kA8: {
      vMaskProcA8Gp(out, flags, msk, mImmutable);
      break;
    }

    case PixelType::kRGBA32: {
      x86::Vec vm = cc->newXmm("c.vm");
      pc->s_mov_i32(vm, msk);
      pc->v_swizzle_lo_u16(vm, vm, x86::shuffleImm(0, 0, 0, 0));

      VecArray vm_(vm);
      vMaskProcRGBA32Vec(out, PixelCount(1), flags, vm_, false, pc->emptyPredicate());
      break;
    }

    default:
      BL_NOT_REACHED();
  }
}

// BLPipeline::JIT::CompOpPart - CMask - Init & Fini - A8
// ======================================================

void CompOpPart::cMaskInitA8(const x86::Gp& sm_, const x86::Vec& vm_) noexcept {
  x86::Gp sm(sm_);
  x86::Vec vm(vm_);

  bool hasMask = sm.isValid() || vm.isValid();
  if (hasMask) {
    // SM must be 32-bit, so make it 32-bit if it's 64-bit for any reason.
    if (sm.isValid())
      sm = sm.r32();

    if (vm.isValid() && !sm.isValid()) {
      sm = cc->newUInt32("sm");
      pc->v_extract_u16(sm, vm, 0);
    }

    _mask->sm = sm;
    _mask->vm = vm;
  }

  if (srcPart()->isSolid()) {
    Pixel& s = srcPart()->as<FetchSolidPart>()->_pixel;
    SolidPixel& o = _solidOpt;
    bool convertToVec = true;

    // CMaskInit - A8 - Solid - SrcCopy
    // --------------------------------

    if (isSrcCopy()) {
      if (!hasMask) {
        // Xa = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);
        o.sa = s.sa;

        if (maxPixels() > 1) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kPA);
          o.px = s.pa[0];
        }

        convertToVec = false;
      }
      else {
        // Xa = (Sa * m) + 0.5 <Rounding>
        // Ya = (1 - m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("p.sx");
        o.sy = sm;

        pc->i_mul(o.sx, s.sa, o.sy);
        pc->i_add(o.sx, o.sx, imm(0x80));
        pc->i_inv_u8(o.sy, o.sy);
      }
    }

    // CMaskInit - A8 - Solid - SrcOver
    // --------------------------------

    else if (isSrcOver()) {
      if (!hasMask) {
        // Xa = Sa * 1 + 0.5 <Rounding>
        // Ya = 1 - Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("p.sx");
        o.sy = sm;

        pc->i_mov(o.sx, s.sa);
        cc->shl(o.sx, 8);
        pc->i_sub(o.sx, o.sx, s.sa);
        pc->i_inv_u8(o.sy, o.sy);
      }
      else {
        // Xa = Sa * m + 0.5 <Rounding>
        // Ya = 1 - (Sa * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("p.sx");
        o.sy = sm;

        pc->i_mul(o.sy, sm, s.sa);
        pc->i_div_255_u32(o.sy, o.sy);

        pc->i_shl(o.sx, o.sy, imm(8));
        pc->i_sub(o.sx, o.sx, o.sy);
        pc->i_add(o.sx, o.sx, imm(0x80));
        pc->i_inv_u8(o.sy, o.sy);
      }
    }

    // CMaskInit - A8 - Solid - SrcIn
    // ------------------------------

    else if (isSrcIn()) {
      if (!hasMask) {
        // Xa = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = s.sa;
        if (maxPixels() > 1) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);
          o.ux = s.ua[0];
        }
      }
      else {
        // Xa = Sa * m + (1 - m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("o.sx");
        pc->i_mul(o.sx, s.sa, sm);
        pc->i_div_255_u32(o.sx, o.sx);
        pc->i_inv_u8(sm, sm);
        pc->i_add(o.sx, o.sx, sm);
      }
    }

    // CMaskInit - A8 - Solid - SrcOut
    // -------------------------------

    else if (isSrcOut()) {
      if (!hasMask) {
        // Xa = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = s.sa;
        if (maxPixels() > 1) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);
          o.ux = s.ua[0];
        }
      }
      else {
        // Xa = Sa * m
        // Ya = 1  - m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("o.sx");
        o.sy = sm;

        pc->i_mul(o.sx, s.sa, o.sy);
        pc->i_div_255_u32(o.sx, o.sx);
        pc->i_inv_u8(o.sy, o.sy);
      }
    }

    // CMaskInit - A8 - Solid - DstOut
    // -------------------------------

    else if (isDstOut()) {
      if (!hasMask) {
        // Xa = 1 - Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("o.sx");
        pc->i_inv_u8(o.sx, s.sa);

        if (maxPixels() > 1) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUI);
          o.ux = s.ui[0];
        }
      }
      else {
        // Xa = 1 - (Sa * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = sm;
        pc->i_mul(o.sx, sm, s.sa);
        pc->i_div_255_u32(o.sx, o.sx);
        pc->i_inv_u8(o.sx, o.sx);
      }
    }

    // CMaskInit - A8 - Solid - Xor
    // ----------------------------

    else if (isXor()) {
      if (!hasMask) {
        // Xa = Sa
        // Ya = 1 - Xa (SIMD only)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);
        o.sx = s.sa;

        if (maxPixels() > 1) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA | PixelFlags::kUI);

          o.ux = s.ua[0];
          o.uy = s.ui[0];
        }
      }
      else {
        // Xa = Sa * m
        // Ya = 1 - Xa (SIMD only)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);

        o.sx = cc->newUInt32("o.sx");
        pc->i_mul(o.sx, sm, s.sa);
        pc->i_div_255_u32(o.sx, o.sx);

        if (maxPixels() > 1) {
          o.ux = pc->newVec("o.ux");
          o.uy = pc->newVec("o.uy");
          pc->v_broadcast_u16(o.ux, o.sx);
          pc->v_inv255_u16(o.uy, o.ux);
        }
      }
    }

    // CMaskInit - A8 - Solid - Plus
    // -----------------------------

    else if (isPlus()) {
      if (!hasMask) {
        // Xa = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA | PixelFlags::kPA);
        o.sa = s.sa;
        o.px = s.pa[0];
        convertToVec = false;
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kSA);
        o.sx = sm;
        pc->i_mul(o.sx, o.sx, s.sa);
        pc->i_div_255_u32(o.sx, o.sx);

        if (maxPixels() > 1) {
          o.px = pc->newVec("o.px");
          pc->i_mul(o.sx, o.sx, 0x01010101);
          pc->v_broadcast_u32(o.px, o.sx);
          pc->i_shr(o.sx, o.sx, imm(24));
        }

        convertToVec = false;
      }
    }

    // CMaskInit - A8 - Solid - Extras
    // -------------------------------

    if (convertToVec && maxPixels() > 1) {
      if (o.sx.isValid() && !o.ux.isValid()) {
        o.ux = pc->newVec("p.ux");
        pc->v_broadcast_u16(o.ux, o.sx);
      }

      if (o.sy.isValid() && !o.uy.isValid()) {
        o.uy = pc->newVec("p.uy");
        pc->v_broadcast_u16(o.uy, o.sy);
      }
    }
  }
  else {
    if (sm.isValid() && !vm.isValid() && maxPixels() > 1) {
      vm = pc->newVec("vm");
      pc->v_broadcast_u16(vm, sm);
      _mask->vm = vm;
    }

    /*
    // CMaskInit - A8 - NonSolid - SrcCopy
    // -----------------------------------

    if (isSrcCopy()) {
      if (hasMask) {
        x86::Vec vn = pc->newVec("vn");
        pc->v_inv255_u16(vn, m);
        _mask->vec.vn = vn;
      }
    }
    */
  }

  _cMaskLoopInit(hasMask ? CMaskLoopType::kVariant : CMaskLoopType::kOpaque);
}

void CompOpPart::cMaskFiniA8() noexcept {
  if (srcPart()->isSolid()) {
    _solidOpt.reset();
    _solidPre.reset();
  }
  else {
    // TODO: [PIPEGEN] ???
  }

  _mask->reset();
  _cMaskLoopFini();
}

// BLPipeline::JIT::CompOpPart - CMask - Proc - A8
// ===============================================

void CompOpPart::cMaskProcA8Gp(Pixel& out, PixelFlags flags) noexcept {
  out.setCount(PixelCount(1));

  bool hasMask = isLoopCMask();

  if (srcPart()->isSolid()) {
    Pixel d("d", pixelType());
    SolidPixel& o = _solidOpt;

    x86::Gp& da = d.sa;
    x86::Gp sx = cc->newUInt32("sx");

    // CMaskProc - A8 - SrcCopy
    // ------------------------

    if (isSrcCopy()) {
      if (!hasMask) {
        // Da' = Xa
        out.sa = o.sa;
        out.makeImmutable();
      }
      else {
        // Da' = Xa  + Da .(1 - m)
        dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

        pc->i_mul(da, da, o.sy),
        pc->i_add(da, da, o.sx);
        pc->i_mul_257_hu16(da, da);

        out.sa = da;
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcOver
    // ------------------------

    if (isSrcOver()) {
      // Da' = Xa + Da .Ya
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(da, da, o.sy);
      pc->i_add(da, da, o.sx);
      pc->i_mul_257_hu16(da, da);

      out.sa = da;

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcIn & DstOut
    // -------------------------------

    if (isSrcIn() || isDstOut()) {
      // Da' = Xa.Da
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(da, da, o.sx);
      pc->i_div_255_u32(da, da);
      out.sa = da;

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcOut
    // -----------------------

    if (isSrcOut()) {
      if (!hasMask) {
        // Da' = Xa.(1 - Da)
        dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

        pc->i_inv_u8(da, da);
        pc->i_mul(da, da, o.sx);
        pc->i_div_255_u32(da, da);
        out.sa = da;
      }
      else {
        // Da' = Xa.(1 - Da) + Da.Ya
        dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

        pc->i_inv_u8(sx, da);
        pc->i_mul(sx, sx, o.sx);
        pc->i_mul(da, da, o.sy);
        pc->i_add(da, da, sx);
        pc->i_div_255_u32(da, da);
        out.sa = da;
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - Xor
    // --------------------

    if (isXor()) {
      // Da' = Xa.(1 - Da) + Da.Ya
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sx, da, o.sy);
      pc->i_inv_u8(da, da);
      pc->i_mul(da, da, o.sx);
      pc->i_add(da, da, sx);
      pc->i_div_255_u32(da, da);
      out.sa = da;

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - Plus
    // ---------------------

    if (isPlus()) {
      // Da' = Clamp(Da + Xa)
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_adds_u8(da, da, o.sx);
      out.sa = da;

      pc->x_satisfy_pixel(out, flags);
      return;
    }
  }

  vMaskProcA8Gp(out, flags, _mask->sm, true);
}

void CompOpPart::cMaskProcA8Vec(Pixel& out, PixelCount n, PixelFlags flags, PixelPredicate& predicate) noexcept {
  out.setCount(n);

  bool hasMask = isLoopCMask();

  if (srcPart()->isSolid()) {
    Pixel d("d", pixelType());
    SolidPixel& o = _solidOpt;

    SimdWidth paSimdWidth = pc->simdWidthOf(DataWidth::k8, n);
    SimdWidth uaSimdWidth = pc->simdWidthOf(DataWidth::k16, n);
    uint32_t kFullN = pc->regCountOf(DataWidth::k16, n);

    VecArray xa;
    pc->newVecArray(xa, kFullN, uaSimdWidth, "x");

    // CMaskProc - A8 - SrcCopy
    // ------------------------

    if (isSrcCopy()) {
      if (!hasMask) {
        // Da' = Xa
        out.pa.init(SimdWidthUtils::cloneVecAs(o.px, paSimdWidth));
        out.makeImmutable();
      }
      else {
        // Da' = Xa + Da .(1 - m)
        dstFetch(d, n, PixelFlags::kUA, predicate);

        x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);
        x86::Vec s_uy = o.uy.cloneAs(d.ua[0]);

        pc->v_mul_i16(d.ua, d.ua, s_uy),
        pc->v_add_i16(d.ua, d.ua, s_ux);
        pc->v_mul257_hi_u16(d.ua, d.ua);

        out.ua.init(d.ua);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcOver
    // ------------------------

    if (isSrcOver()) {
      // Da' = Xa + Da.Ya
      dstFetch(d, n, PixelFlags::kUA, predicate);

      x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);
      x86::Vec s_uy = o.uy.cloneAs(d.ua[0]);

      pc->v_mul_i16(d.ua, d.ua, s_uy);
      pc->v_add_i16(d.ua, d.ua, s_ux);
      pc->v_mul257_hi_u16(d.ua, d.ua);

      out.ua.init(d.ua);

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcIn & DstOut
    // -------------------------------

    if (isSrcIn() || isDstOut()) {
      // Da' = Xa.Da
      dstFetch(d, n, PixelFlags::kUA, predicate);

      x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);

      pc->v_mul_u16(d.ua, d.ua, s_ux);
      pc->v_div255_u16(d.ua);
      out.ua.init(d.ua);

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - SrcOut
    // -----------------------

    if (isSrcOut()) {
      if (!hasMask) {
        // Da' = Xa.(1 - Da)
        dstFetch(d, n, PixelFlags::kUA, predicate);

        x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);

        pc->v_inv255_u16(d.ua, d.ua);
        pc->v_mul_u16(d.ua, d.ua, s_ux);
        pc->v_div255_u16(d.ua);
        out.ua.init(d.ua);
      }
      else {
        // Da' = Xa.(1 - Da) + Da.Ya
        dstFetch(d, n, PixelFlags::kUA, predicate);

        x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);
        x86::Vec s_uy = o.uy.cloneAs(d.ua[0]);

        pc->v_inv255_u16(xa, d.ua);
        pc->v_mul_u16(xa, xa, s_ux);
        pc->v_mul_u16(d.ua, d.ua, s_uy);
        pc->v_add_i16(d.ua, d.ua, xa);
        pc->v_div255_u16(d.ua);
        out.ua.init(d.ua);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - Xor
    // --------------------

    if (isXor()) {
      // Da' = Xa.(1 - Da) + Da.Ya
      dstFetch(d, n, PixelFlags::kUA, predicate);

      x86::Vec s_ux = o.ux.cloneAs(d.ua[0]);
      x86::Vec s_uy = o.uy.cloneAs(d.ua[0]);

      pc->v_mul_u16(xa, d.ua, s_uy);
      pc->v_inv255_u16(d.ua, d.ua);
      pc->v_mul_u16(d.ua, d.ua, s_ux);
      pc->v_add_i16(d.ua, d.ua, xa);
      pc->v_div255_u16(d.ua);
      out.ua.init(d.ua);

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - A8 - Plus
    // ---------------------

    if (isPlus()) {
      // Da' = Clamp(Da + Xa)
      dstFetch(d, n, PixelFlags::kPA, predicate);

      x86::Vec s_px = o.px.cloneAs(d.pa[0]);

      pc->v_adds_u8(d.pa, d.pa, s_px);
      out.pa.init(d.pa);

      pc->x_satisfy_pixel(out, flags);
      return;
    }
  }

  VecArray vm;
  if (_mask->vm.isValid())
    vm.init(_mask->vm);
  vMaskProcA8Vec(out, n, flags, vm, true, predicate);
}

// BLPipeline::JIT::CompOpPart - VMask Proc - A8 (Scalar)
// ======================================================

void CompOpPart::vMaskProcA8Gp(Pixel& out, PixelFlags flags, x86::Gp& msk, bool mImmutable) noexcept {
  bool hasMask = msk.isValid();

  Pixel d("d", PixelType::kA8);
  Pixel s("s", PixelType::kA8);

  x86::Gp x = cc->newUInt32("@x");
  x86::Gp y = cc->newUInt32("@y");

  x86::Gp& da = d.sa;
  x86::Gp& sa = s.sa;

  out.setCount(PixelCount(1));

  // VMask - A8 - SrcCopy
  // --------------------

  if (isSrcCopy()) {
    if (!hasMask) {
      // Da' = Sa
      srcFetch(out, PixelCount(1), flags, pc->emptyPredicate());
    }
    else {
      // Da' = Sa.m + Da.(1 - m)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_inv_u8(msk, msk);
      pc->i_mul(da, da, msk);

      if (mImmutable)
        pc->i_inv_u8(msk, msk);

      pc->i_add(da, da, sa);
      pc->i_div_255_u32(da, da);

      out.sa = da;
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcOver
  // --------------------

  if (isSrcOver()) {
    if (!hasMask) {
      // Da' = Sa + Da.(1 - Sa)
      srcFetch(s, PixelCount(1), PixelFlags::kSA | PixelFlags::kImmutable, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_inv_u8(x, sa);
      pc->i_mul(da, da, x);
      pc->i_div_255_u32(da, da);
      pc->i_add(da, da, sa);
    }
    else {
      // Da' = Sa.m + Da.(1 - Sa.m)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);
      pc->i_inv_u8(x, sa);
      pc->i_mul(da, da, x);
      pc->i_div_255_u32(da, da);
      pc->i_add(da, da, sa);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcIn
  // ------------------

  if (isSrcIn()) {
    if (!hasMask) {
      // Da' = Sa.Da
      srcFetch(s, PixelCount(1), PixelFlags::kSA | PixelFlags::kImmutable, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(da, da, sa);
      pc->i_div_255_u32(da, da);
    }
    else {
      // Da' = Da.(Sa.m) + Da.(1 - m)
      //     = Da.(Sa.m + 1 - m)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);
      pc->i_add(sa, sa, imm(255));
      pc->i_sub(sa, sa, msk);
      pc->i_mul(da, da, sa);
      pc->i_div_255_u32(da, da);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcOut
  // -------------------

  if (isSrcOut()) {
    if (!hasMask) {
      // Da' = Sa.(1 - Da)
      srcFetch(s, PixelCount(1), PixelFlags::kSA | PixelFlags::kImmutable, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_inv_u8(da, da);
      pc->i_mul(da, da, sa);
      pc->i_div_255_u32(da, da);
    }
    else {
      // Da' = Sa.m.(1 - Da) + Da.(1 - m)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);

      pc->i_inv_u8(x, da);
      pc->i_inv_u8(msk, msk);
      pc->i_mul(sa, sa, x);
      pc->i_mul(da, da, msk);

      if (mImmutable)
        pc->i_inv_u8(msk, msk);

      pc->i_add(da, da, sa);
      pc->i_div_255_u32(da, da);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - DstOut
  // -------------------

  if (isDstOut()) {
    if (!hasMask) {
      // Da' = Da.(1 - Sa)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_inv_u8(sa, sa);
      pc->i_mul(da, da, sa);
      pc->i_div_255_u32(da, da);
    }
    else {
      // Da' = Da.(1 - Sa.m)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);
      pc->i_inv_u8(sa, sa);
      pc->i_mul(da, da, sa);
      pc->i_div_255_u32(da, da);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Xor
  // ----------------

  if (isXor()) {
    if (!hasMask) {
      // Da' = Da.(1 - Sa) + Sa.(1 - Da)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_inv_u8(y, sa);
      pc->i_inv_u8(x, da);

      pc->i_mul(da, da, y);
      pc->i_mul(sa, sa, x);
      pc->i_add(da, da, sa);
      pc->i_div_255_u32(da, da);
    }
    else {
      // Da' = Da.(1 - Sa.m) + Sa.m.(1 - Da)
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);

      pc->i_inv_u8(y, sa);
      pc->i_inv_u8(x, da);

      pc->i_mul(da, da, y);
      pc->i_mul(sa, sa, x);
      pc->i_add(da, da, sa);
      pc->i_div_255_u32(da, da);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Plus
  // -----------------

  if (isPlus()) {
    // Da' = Clamp(Da + Sa)
    // Da' = Clamp(Da + Sa.m)
    if (hasMask) {
      srcFetch(s, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());

      pc->i_mul(sa, sa, msk);
      pc->i_div_255_u32(sa, sa);
    }
    else {
      srcFetch(s, PixelCount(1), PixelFlags::kSA | PixelFlags::kImmutable, pc->emptyPredicate());
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
    }

    pc->i_adds_u8(da, da, sa);

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Invert
  // -------------------

  if (compOp() == BL_COMP_OP_INTERNAL_ALPHA_INV) {
    // Da' = 1 - Da
    // Da' = Da.(1 - m) + (1 - Da).m
    if (hasMask) {
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      pc->i_inv_u8(x, msk);
      pc->i_mul(x, x, da);
      pc->i_inv_u8(da, da);
      pc->i_mul(da, da, msk);
      pc->i_add(da, da, x);
      pc->i_div_255_u32(da, da);
    }
    else {
      dstFetch(d, PixelCount(1), PixelFlags::kSA, pc->emptyPredicate());
      pc->i_inv_u8(da, da);
    }

    out.sa = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Invalid
  // --------------------

  BL_NOT_REACHED();
}

// BLPipeline::JIT::CompOpPart - VMask - Proc - A8 (Vec)
// =====================================================

void CompOpPart::vMaskProcA8Vec(Pixel& out, PixelCount n, PixelFlags flags, VecArray& vm_, bool mImmutable, PixelPredicate& predicate) noexcept {
  SimdWidth simdWidth = pc->simdWidthOf(DataWidth::k16, n);
  uint32_t kFullN = pc->regCountOf(DataWidth::k16, n);

  VecArray vm = vm_.cloneAs(simdWidth);
  bool hasMask = !vm.empty();

  Pixel d("d", PixelType::kA8);
  Pixel s("s", PixelType::kA8);

  VecArray& da = d.ua;
  VecArray& sa = s.ua;

  VecArray xv, yv;
  pc->newVecArray(xv, kFullN, simdWidth, "x");
  pc->newVecArray(yv, kFullN, simdWidth, "y");

  out.setCount(n);

  // VMask - A8 - SrcCopy
  // --------------------

  if (isSrcCopy()) {
    if (!hasMask) {
      // Da' = Sa
      srcFetch(out, n, flags, predicate);
    }
    else {
      // Da' = Sa.m + Da.(1 - m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_inv255_u16(vm, vm);
      pc->v_mul_u16(da, da, vm);

      if (mImmutable)
        pc->v_inv255_u16(vm, vm);

      pc->v_add_i16(da, da, sa);
      pc->v_div255_u16(da);

      out.ua = da;
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcOver
  // --------------------

  if (isSrcOver()) {
    if (!hasMask) {
      // Da' = Sa + Da.(1 - Sa)
      srcFetch(s, n, PixelFlags::kUA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_inv255_u16(xv, sa);
      pc->v_mul_u16(da, da, xv);
      pc->v_div255_u16(da);
      pc->v_add_i16(da, da, sa);
    }
    else {
      // Da' = Sa.m + Da.(1 - Sa.m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_div255_u16(sa);
      pc->v_inv255_u16(xv, sa);
      pc->v_mul_u16(da, da, xv);
      pc->v_div255_u16(da);
      pc->v_add_i16(da, da, sa);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcIn
  // ------------------

  if (isSrcIn()) {
    if (!hasMask) {
      // Da' = Sa.Da
      srcFetch(s, n, PixelFlags::kUA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(da, da, sa);
      pc->v_div255_u16(da);
    }
    else {
      // Da' = Da.(Sa.m) + Da.(1 - m)
      //     = Da.(Sa.m + 1 - m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_div255_u16(sa);
      pc->v_add_i16(sa, sa, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, sa));
      pc->v_sub_i16(sa, sa, vm);
      pc->v_mul_u16(da, da, sa);
      pc->v_div255_u16(da);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - SrcOut
  // -------------------

  if (isSrcOut()) {
    if (!hasMask) {
      // Da' = Sa.(1 - Da)
      srcFetch(s, n, PixelFlags::kUA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_inv255_u16(da, da);
      pc->v_mul_u16(da, da, sa);
      pc->v_div255_u16(da);
    }
    else {
      // Da' = Sa.m.(1 - Da) + Da.(1 - m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_div255_u16(sa);

      pc->v_inv255_u16(xv, da);
      pc->v_inv255_u16(vm, vm);
      pc->v_mul_u16(sa, sa, xv);
      pc->v_mul_u16(da, da, vm);

      if (mImmutable)
        pc->v_inv255_u16(vm, vm);

      pc->v_add_i16(da, da, sa);
      pc->v_div255_u16(da);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - DstOut
  // -------------------

  if (isDstOut()) {
    if (!hasMask) {
      // Da' = Da.(1 - Sa)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_inv255_u16(sa, sa);
      pc->v_mul_u16(da, da, sa);
      pc->v_div255_u16(da);
    }
    else {
      // Da' = Da.(1 - Sa.m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_div255_u16(sa);
      pc->v_inv255_u16(sa, sa);
      pc->v_mul_u16(da, da, sa);
      pc->v_div255_u16(da);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Xor
  // ----------------

  if (isXor()) {
    if (!hasMask) {
      // Da' = Da.(1 - Sa) + Sa.(1 - Da)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_inv255_u16(yv, sa);
      pc->v_inv255_u16(xv, da);

      pc->v_mul_u16(da, da, yv);
      pc->v_mul_u16(sa, sa, xv);
      pc->v_add_i16(da, da, sa);
      pc->v_div255_u16(da);
    }
    else {
      // Da' = Da.(1 - Sa.m) + Sa.m.(1 - Da)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(sa, sa, vm);
      pc->v_div255_u16(sa);

      pc->v_inv255_u16(yv, sa);
      pc->v_inv255_u16(xv, da);

      pc->v_mul_u16(da, da, yv);
      pc->v_mul_u16(sa, sa, xv);
      pc->v_add_i16(da, da, sa);
      pc->v_div255_u16(da);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Plus
  // -----------------

  if (isPlus()) {
    if (!hasMask) {
      // Da' = Clamp(Da + Sa)
      srcFetch(s, n, PixelFlags::kPA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kPA, predicate);

      pc->v_adds_u8(d.pa, d.pa, s.pa);
      out.pa = d.pa;
    }
    else {
      // Da' = Clamp(Da + Sa.m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      pc->v_mul_u16(s.ua, s.ua, vm);
      pc->v_div255_u16(s.ua);
      pc->v_adds_u8(d.ua, d.ua, s.ua);
      out.ua = d.ua;
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Invert
  // -------------------

  if (compOp() == BL_COMP_OP_INTERNAL_ALPHA_INV) {
    if (!hasMask) {
      // Da' = 1 - Da
      dstFetch(d, n, PixelFlags::kUA, predicate);
      pc->v_inv255_u16(da, da);
    }
    else {
      // Da' = Da.(1 - m) + (1 - Da).m
      dstFetch(d, n, PixelFlags::kUA, predicate);
      pc->v_inv255_u16(xv, vm);
      pc->v_mul_u16(xv, xv, da);
      pc->v_inv255_u16(da, da);
      pc->v_mul_u16(da, da, vm);
      pc->v_add_i16(da, da, xv);
      pc->v_div255_u16(da);
    }

    out.ua = da;
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMask - A8 - Invalid
  // --------------------

  BL_NOT_REACHED();
}

// BLPipeline::JIT::CompOpPart - CMask - Init & Fini - RGBA
// ========================================================

void CompOpPart::cMaskInitRGBA32(const x86::Vec& vm) noexcept {
  bool hasMask = vm.isValid();
  bool useDa = hasDa();

  if (srcPart()->isSolid()) {
    Pixel& s = srcPart()->as<FetchSolidPart>()->_pixel;
    SolidPixel& o = _solidOpt;

    // CMaskInit - RGBA32 - Solid - SrcCopy
    // ------------------------------------

    if (isSrcCopy()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kPC);

        o.px = s.pc[0];
      }
      else {
        // Xca = (Sca * m) + 0.5 <Rounding>
        // Xa  = (Sa  * m) + 0.5 <Rounding>
        // Im  = (1 - m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.vn = vm;

        pc->v_mul_u16(o.ux, s.uc[0], o.vn);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_0080008000800080, Bcst::kNA, o.ux));
        pc->v_inv255_u16(o.vn, o.vn);
      }
    }

    // CMaskInit - RGBA32 - Solid - SrcOver
    // ------------------------------------

    else if (isSrcOver()) {
      if (!hasMask) {
        // Xca = Sca * 1 + 0.5 <Rounding>
        // Xa  = Sa  * 1 + 0.5 <Rounding>
        // Yca = 1 - Sa
        // Ya  = 1 - Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUI | PixelFlags::kImmutable);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = s.ui[0];

        pc->v_sll_i16(o.ux, s.uc[0], 8);
        pc->v_sub_i16(o.ux, o.ux, s.uc[0]);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_0080008000800080, Bcst::kNA, o.ux));
      }
      else {
        // Xca = Sca * m + 0.5 <Rounding>
        // Xa  = Sa  * m + 0.5 <Rounding>
        // Yca = 1 - (Sa * m)
        // Ya  = 1 - (Sa * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kImmutable);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");

        pc->v_mul_u16(o.uy, s.uc[0], vm);
        pc->v_div255_u16(o.uy);

        pc->v_sll_i16(o.ux, o.uy, 8);
        pc->v_sub_i16(o.ux, o.ux, o.uy);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_0080008000800080, Bcst::kNA, o.ux));

        pc->v_expand_alpha_16(o.uy, o.uy);
        pc->v_inv255_u16(o.uy, o.uy);
      }
    }

    // CMaskInit - RGBA32 - Solid - SrcIn | SrcOut
    // -------------------------------------------

    else if (isSrcIn() || isSrcOut()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = s.uc[0];
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        // Im  = 1   - m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.vn = vm;

        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_div255_u16(o.ux);
        pc->v_inv255_u16(vm, vm);
      }
    }

    // CMaskInit - RGBA32 - Solid - SrcAtop & Xor & Darken & Lighten
    // -------------------------------------------------------------

    else if (isSrcAtop() || isXor() || isDarken() || isLighten()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        // Yca = 1 - Sa
        // Ya  = 1 - Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUI);

        o.ux = s.uc[0];
        o.uy = s.ui[0];
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        // Yca = 1 - (Sa * m)
        // Ya  = 1 - (Sa * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = vm;

        pc->v_mul_u16(o.ux, s.uc[0], o.uy);
        pc->v_div255_u16(o.ux);

        pc->v_expand_alpha_16(o.uy, o.ux, false);
        pc->v_swizzle_u32(o.uy, o.uy, x86::shuffleImm(0, 0, 0, 0));
        pc->v_inv255_u16(o.uy, o.uy);
      }
    }

    // CMaskInit - RGBA32 - Solid - Dst
    // --------------------------------

    else if (isDstCopy()) {
      BL_NOT_REACHED();
    }

    // CMaskInit - RGBA32 - Solid - DstOver
    // ------------------------------------

    else if (isDstOver()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = s.uc[0];
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_div255_u16(o.ux);
      }
    }

    // CMaskInit - RGBA32 - Solid - DstIn
    // ----------------------------------

    else if (isDstIn()) {
      if (!hasMask) {
        // Xca = Sa
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);

        o.ux = s.ua[0];
      }
      else {
        // Xca = 1 - m.(1 - Sa)
        // Xa  = 1 - m.(1 - Sa)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);

        o.ux = cc->newSimilarReg(s.ua[0], "solid.ux");
        pc->v_mov(o.ux, s.ua[0]);
        pc->v_inv255_u16(o.ux, o.ux);
        pc->v_mul_u16(o.ux, o.ux, vm);
        pc->v_div255_u16(o.ux);
        pc->v_inv255_u16(o.ux, o.ux);
      }
    }

    // CMaskInit - RGBA32 - Solid - DstOut
    // -----------------------------------

    else if (isDstOut()) {
      if (!hasMask) {
        // Xca = 1 - Sa
        // Xa  = 1 - Sa
        if (useDa) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUI);

          o.ux = s.ui[0];
        }
        // Xca = 1 - Sa
        // Xa  = 1
        else {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);

          o.ux = cc->newSimilarReg(s.ua[0], "solid.ux");
          pc->v_mov(o.ux, s.ua[0]);
          pc->vNegRgb8W(o.ux, o.ux);
        }
      }
      else {
        // Xca = 1 - (Sa * m)
        // Xa  = 1 - (Sa * m)
        if (useDa) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);

          o.ux = vm;
          pc->v_mul_u16(o.ux, o.ux, s.ua[0]);
          pc->v_div255_u16(o.ux);
          pc->v_inv255_u16(o.ux, o.ux);
        }
        // Xca = 1 - (Sa * m)
        // Xa  = 1
        else {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUA);

          o.ux = vm;
          pc->v_mul_u16(o.ux, o.ux, s.ua[0]);
          pc->v_div255_u16(o.ux);
          pc->v_inv255_u16(o.ux, o.ux);
          pc->vFillAlpha255W(o.ux, o.ux);
        }
      }
    }

    // CMaskInit - RGBA32 - Solid - DstAtop
    // ------------------------------------

    else if (isDstAtop()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        // Yca = Sa
        // Ya  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUA);

        o.ux = s.uc[0];
        o.uy = s.ua[0];
      }
      else {
        // Xca = Sca.m
        // Xa  = Sa .m
        // Yca = Sa .m + (1 - m)
        // Ya  = Sa .m + (1 - m)

        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");
        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_inv255_u16(o.uy, vm);
        pc->v_div255_u16(o.ux);
        pc->v_add_i16(o.uy, o.uy, o.ux);
        pc->v_expand_alpha_16(o.uy, o.uy);
      }
    }

    // CMaskInit - RGBA32 - Solid - Plus
    // ---------------------------------

    else if (isPlus()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kPC);

        o.px = s.pc[0];
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.px = cc->newSimilarReg(s.pc[0], "solid.px");
        pc->v_mul_u16(o.px, s.uc[0], vm);
        pc->v_div255_u16(o.px);
        pc->v_packs_i16_u8(o.px, o.px, o.px);
      }
    }

    // CMaskInit - RGBA32 - Solid - Minus
    // ----------------------------------

    else if (isMinus()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = 0
        // Yca = Sca
        // Ya  = Sa
        if (useDa) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

          o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
          o.uy = s.uc[0];
          pc->v_mov(o.ux, o.uy);
          pc->vZeroAlphaW(o.ux, o.ux);
        }
        else {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kPC);

          o.px = cc->newSimilarReg(s.pc[0], "solid.px");
          pc->v_mov(o.px, s.pc[0]);
          pc->vZeroAlphaB(o.px, o.px);
        }
      }
      else {
        // Xca = Sca
        // Xa  = 0
        // Yca = Sca
        // Ya  = Sa
        // M   = m       <Alpha channel is set to 256>
        // N   = 1 - m   <Alpha channel is set to 0  >
        if (useDa) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

          o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
          o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");
          o.vm = vm;
          o.vn = cc->newSimilarReg(s.uc[0], "vn");

          pc->vZeroAlphaW(o.ux, s.uc[0]);
          pc->v_mov(o.uy, s.uc[0]);

          pc->v_inv255_u16(o.vn, o.vm);
          pc->vZeroAlphaW(o.vm, o.vm);
          pc->vZeroAlphaW(o.vn, o.vn);
          pc->vFillAlpha255W(o.vm, o.vm);
        }
        else {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

          o.ux = cc->newSimilarReg(s.uc[0], "ux");
          o.vm = vm;
          o.vn = cc->newSimilarReg(s.uc[0], "vn");
          pc->vZeroAlphaW(o.ux, s.uc[0]);
          pc->v_inv255_u16(o.vn, o.vm);
        }
      }
    }

    // CMaskInit - RGBA32 - Solid - Modulate
    // -------------------------------------

    else if (isModulate()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = s.uc[0];
      }
      else {
        // Xca = Sca * m + (1 - m)
        // Xa  = Sa  * m + (1 - m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_div255_u16(o.ux);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, o.ux));
        pc->v_sub_i16(o.ux, o.ux, vm);
      }
    }

    // CMaskInit - RGBA32 - Solid - Multiply
    // -------------------------------------

    else if (isMultiply()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        // Yca = Sca + (1 - Sa)
        // Ya  = Sa  + (1 - Sa)
        if (useDa) {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUI);

          o.ux = s.uc[0];
          o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");

          pc->v_mov(o.uy, s.ui[0]);
          pc->v_add_i16(o.uy, o.uy, o.ux);
        }
        // Yca = Sca + (1 - Sa)
        // Ya  = Sa  + (1 - Sa)
        else {
          srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUI);

          o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");
          pc->v_mov(o.uy, s.ui[0]);
          pc->v_add_i16(o.uy, o.uy, s.uc[0]);
        }
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        // Yca = Sca * m + (1 - Sa * m)
        // Ya  = Sa  * m + (1 - Sa * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");

        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_div255_u16(o.ux);
        pc->v_swizzle_lo_u16(o.uy, o.ux, x86::shuffleImm(3, 3, 3, 3));
        pc->v_inv255_u16(o.uy, o.uy);
        pc->v_swizzle_u32(o.uy, o.uy, x86::shuffleImm(0, 0, 0, 0));
        pc->v_add_i16(o.uy, o.uy, o.ux);
      }
    }

    // CMaskInit - RGBA32 - Solid - Screen
    // -----------------------------------

    else if (isScreen()) {
      if (!hasMask) {
        // Xca = Sca * 1 + 0.5 <Rounding>
        // Xa  = Sa  * 1 + 0.5 <Rounding>
        // Yca = 1 - Sca
        // Ya  = 1 - Sa

        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");

        pc->v_inv255_u16(o.uy, o.ux);
        pc->v_sll_i16(o.ux, s.uc[0], 8);
        pc->v_sub_i16(o.ux, o.ux, s.uc[0]);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_0080008000800080, Bcst::kNA, o.ux));
      }
      else {
        // Xca = Sca * m + 0.5 <Rounding>
        // Xa  = Sa  * m + 0.5 <Rounding>
        // Yca = 1 - (Sca * m)
        // Ya  = 1 - (Sa  * m)
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "solid.ux");
        o.uy = cc->newSimilarReg(s.uc[0], "solid.uy");

        pc->v_mul_u16(o.uy, s.uc[0], vm);
        pc->v_div255_u16(o.uy);
        pc->v_sll_i16(o.ux, o.uy, 8);
        pc->v_sub_i16(o.ux, o.ux, o.uy);
        pc->v_add_i16(o.ux, o.ux, pc->simdConst(&ct.i_0080008000800080, Bcst::kNA, o.ux));
        pc->v_inv255_u16(o.uy, o.uy);
      }
    }

    // CMaskInit - RGBA32 - Solid - LinearBurn & Difference & Exclusion
    // ----------------------------------------------------------------

    else if (isLinearBurn() || isDifference() || isExclusion()) {
      if (!hasMask) {
        // Xca = Sca
        // Xa  = Sa
        // Yca = Sa
        // Ya  = Sa
        srcPart()->as<FetchSolidPart>()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC | PixelFlags::kUA);

        o.ux = s.uc[0];
        o.uy = s.ua[0];
      }
      else {
        // Xca = Sca * m
        // Xa  = Sa  * m
        // Yca = Sa  * m
        // Ya  = Sa  * m
        srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

        o.ux = cc->newSimilarReg(s.uc[0], "ux");
        o.uy = cc->newSimilarReg(s.uc[0], "uy");

        pc->v_mul_u16(o.ux, s.uc[0], vm);
        pc->v_div255_u16(o.ux);
        pc->v_swizzle_lo_u16(o.uy, o.ux, x86::shuffleImm(3, 3, 3, 3));
        pc->v_swizzle_u32(o.uy, o.uy, x86::shuffleImm(0, 0, 0, 0));
      }
    }

    // CMaskInit - RGBA32 - Solid - TypeA (Non-Opaque)
    // -----------------------------------------------

    else if (blTestFlag(compOpFlags(), BLCompOpFlags::kTypeA) && hasMask) {
      // Multiply the source pixel with the mask if `TypeA`.
      srcPart()->as<FetchSolidPart>()->initSolidFlags(PixelFlags::kUC);

      Pixel& pre = _solidPre;
      pre.setCount(PixelCount(1));
      pre.uc.init(cc->newSimilarReg(s.uc[0], "pre.uc"));

      pc->v_mul_u16(pre.uc[0], s.uc[0], vm);
      pc->v_div255_u16(pre.uc[0]);
    }

    // CMaskInit - RGBA32 - Solid - No Optimizations
    // ---------------------------------------------

    else {
      // No optimization. The compositor will simply use the mask provided.
      _mask->vm = vm;
    }
  }
  else {
    _mask->vm = vm;

    // CMaskInit - RGBA32 - NonSolid - SrcCopy
    // ---------------------------------------

    if (isSrcCopy()) {
      if (hasMask) {
        _mask->vn = cc->newSimilarReg(vm, "vn");
        pc->v_inv255_u16(_mask->vn, vm);
      }
    }
  }

  _cMaskLoopInit(hasMask ? CMaskLoopType::kVariant : CMaskLoopType::kOpaque);
}

void CompOpPart::cMaskFiniRGBA32() noexcept {
  if (srcPart()->isSolid()) {
    _solidOpt.reset();
    _solidPre.reset();
  }
  else {
    // TODO: [PIPEGEN]
  }

  _mask->reset();
  _cMaskLoopFini();
}

// BLPipeline::JIT::CompOpPart - CMask - Proc - RGBA
// =================================================

void CompOpPart::cMaskProcRGBA32Vec(Pixel& out, PixelCount n, PixelFlags flags, PixelPredicate& predicate) noexcept {
  bool hasMask = isLoopCMask();

  SimdWidth simdWidth = pc->simdWidthOf(DataWidth::k64, n);
  uint32_t kFullN = pc->regCountOf(DataWidth::k64, n);
  uint32_t kUseHi = n > 1;

  out.setCount(n);

  if (srcPart()->isSolid()) {
    Pixel d("d", pixelType());
    SolidPixel& o = _solidOpt;

    VecArray xv, yv, zv;
    pc->newVecArray(xv, kFullN, simdWidth, "x");
    pc->newVecArray(yv, kFullN, simdWidth, "y");
    pc->newVecArray(zv, kFullN, simdWidth, "z");

    bool useDa = hasDa();

    // CMaskProc - RGBA32 - SrcCopy
    // ----------------------------

    if (isSrcCopy()) {
      // Dca' = Xca
      // Da'  = Xa
      if (!hasMask) {
        out.pc = VecArray(o.px).cloneAs(simdWidth);
        out.makeImmutable();
      }
      // Dca' = Xca + Dca.(1 - m)
      // Da'  = Xa  + Da .(1 - m)
      else {
        dstFetch(d, n, PixelFlags::kUC, predicate);
        VecArray& dv = d.uc;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);
        x86::Vec s_vn = o.vn.cloneAs(dv[0]);

        pc->v_mul_u16(dv, dv, s_vn);
        pc->v_add_i16(dv, dv, s_ux);
        pc->v_mul257_hi_u16(dv, dv);
        out.uc.init(dv);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - SrcOver & Screen
    // -------------------------------------

    if (isSrcOver() || isScreen()) {
      // Dca' = Xca + Dca.Yca
      // Da'  = Xa  + Da .Ya
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);
      x86::Vec s_uy = o.uy.cloneAs(dv[0]);

      pc->v_mul_u16(dv, dv, s_uy);
      pc->v_add_i16(dv, dv, s_ux);
      pc->v_mul257_hi_u16(dv, dv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);

      return;
    }

    // CMaskProc - RGBA32 - SrcIn
    // --------------------------

    if (isSrcIn()) {
      // Dca' = Xca.Da
      // Da'  = Xa .Da
      if (!hasMask) {
        dstFetch(d, n, PixelFlags::kUA, predicate);
        VecArray& dv = d.ua;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);

        pc->v_mul_u16(dv, dv, s_ux);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      // Dca' = Xca.Da + Dca.(1 - m)
      // Da'  = Xa .Da + Da .(1 - m)
      else {
        dstFetch(d, n, PixelFlags::kUC | PixelFlags::kUA, predicate);
        VecArray& dv = d.uc;
        VecArray& da = d.ua;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);
        x86::Vec s_vn = o.vn.cloneAs(dv[0]);

        pc->v_mul_u16(dv, dv, s_vn);
        pc->v_mul_u16(da, da, s_ux);
        pc->v_add_i16(dv, dv, da);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - SrcOut
    // ---------------------------

    if (isSrcOut()) {
      // Dca' = Xca.(1 - Da)
      // Da'  = Xa .(1 - Da)
      if (!hasMask) {
        dstFetch(d, n, PixelFlags::kUI, predicate);
        VecArray& dv = d.ui;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);

        pc->v_mul_u16(dv, dv, s_ux);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      // Dca' = Xca.(1 - Da) + Dca.(1 - m)
      // Da'  = Xa .(1 - Da) + Da .(1 - m)
      else {
        dstFetch(d, n, PixelFlags::kUC, predicate);
        VecArray& dv = d.uc;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);
        x86::Vec s_vn = o.vn.cloneAs(dv[0]);

        pc->v_expand_alpha_16(xv, dv, kUseHi);
        pc->v_inv255_u16(xv, xv);
        pc->v_mul_u16(xv, xv, s_ux);
        pc->v_mul_u16(dv, dv, s_vn);
        pc->v_add_i16(dv, dv, xv);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - SrcAtop
    // ----------------------------

    if (isSrcAtop()) {
      // Dca' = Xca.Da + Dca.Yca
      // Da'  = Xa .Da + Da .Ya
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);
      x86::Vec s_uy = o.uy.cloneAs(dv[0]);

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(dv, dv, s_uy);
      pc->v_mul_u16(xv, xv, s_ux);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Dst
    // ------------------------

    if (isDstCopy()) {
      // Dca' = Dca
      // Da'  = Da
      BL_NOT_REACHED();
    }

    // CMaskProc - RGBA32 - DstOver
    // ----------------------------

    if (isDstOver()) {
      // Dca' = Xca.(1 - Da) + Dca
      // Da'  = Xa .(1 - Da) + Da
      dstFetch(d, n, PixelFlags::kPC | PixelFlags::kUI, predicate);
      VecArray& dv = d.ui;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);

      pc->v_mul_u16(dv, dv, s_ux);
      pc->v_div255_u16(dv);

      VecArray dh;
      if (pc->hasAVX()) {
        pc->_x_pack_pixel(dh, dv, n.value() * 4, "", "d");
      }
      else {
        dh = dv.even();
        pc->x_packs_i16_u8(dh, dh, dv.odd());
      }

      dh = dh.cloneAs(d.pc[0]);
      pc->v_add_i32(dh, dh, d.pc);

      out.pc.init(dh);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - DstIn & DstOut
    // -----------------------------------

    if (isDstIn() || isDstOut()) {
      // Dca' = Xca.Dca
      // Da'  = Xa .Da
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);

      pc->v_mul_u16(dv, dv, s_ux);
      pc->v_div255_u16(dv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - DstAtop | Xor | Multiply
    // ---------------------------------------------

    if (isDstAtop() || isXor() || isMultiply()) {
      if (useDa) {
        // Dca' = Xca.(1 - Da) + Dca.Yca
        // Da'  = Xa .(1 - Da) + Da .Ya
        dstFetch(d, n, PixelFlags::kUC, predicate);
        VecArray& dv = d.uc;

        x86::Vec s_ux = o.ux.cloneAs(dv[0]);
        x86::Vec s_uy = o.uy.cloneAs(dv[0]);

        pc->v_expand_alpha_16(xv, dv, kUseHi);
        pc->v_mul_u16(dv, dv, s_uy);
        pc->v_inv255_u16(xv, xv);
        pc->v_mul_u16(xv, xv, s_ux);

        pc->v_add_i16(dv, dv, xv);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      else {
        // Dca' = Dca.Yca
        // Da'  = Da .Ya
        dstFetch(d, n, PixelFlags::kUC, predicate);
        VecArray& dv = d.uc;

        x86::Vec s_uy = o.uy.cloneAs(dv[0]);

        pc->v_mul_u16(dv, dv, s_uy);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Plus
    // -------------------------

    if (isPlus()) {
      // Dca' = Clamp(Dca + Sca)
      // Da'  = Clamp(Da  + Sa )
      dstFetch(d, n, PixelFlags::kPC, predicate);
      VecArray& dv = d.pc;

      x86::Vec s_px = o.px.cloneAs(dv[0]);

      pc->v_adds_u8(dv, dv, s_px);

      out.pc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Minus
    // --------------------------

    if (isMinus()) {
      if (!hasMask) {
        if (useDa) {
          // Dca' = Clamp(Dca - Xca) + Yca.(1 - Da)
          // Da'  = Da + Ya.(1 - Da)
          dstFetch(d, n, PixelFlags::kUC, predicate);
          VecArray& dv = d.uc;

          x86::Vec s_ux = o.ux.cloneAs(dv[0]);
          x86::Vec s_uy = o.uy.cloneAs(dv[0]);

          pc->v_expand_alpha_16(xv, dv, kUseHi);
          pc->v_inv255_u16(xv, xv);
          pc->v_mul_u16(xv, xv, s_uy);
          pc->v_subs_u16(dv, dv, s_ux);
          pc->v_div255_u16(xv);

          pc->v_add_i16(dv, dv, xv);
          out.uc.init(dv);
        }
        else {
          // Dca' = Clamp(Dca - Xca)
          // Da'  = <unchanged>
          dstFetch(d, n, PixelFlags::kPC, predicate);
          VecArray& dh = d.pc;

          x86::Vec s_px = o.px.cloneAs(dh[0]);

          pc->v_subs_u8(dh, dh, s_px);
          out.pc.init(dh);
        }
      }
      else {
        if (useDa) {
          // Dca' = (Clamp(Dca - Xca) + Yca.(1 - Da)).m + Dca.(1 - m)
          // Da'  = Da + Ya.(1 - Da)
          dstFetch(d, n, PixelFlags::kUC, predicate);
          VecArray& dv = d.uc;

          x86::Vec s_ux = o.ux.cloneAs(dv[0]);
          x86::Vec s_uy = o.uy.cloneAs(dv[0]);
          x86::Vec s_vn = o.vn.cloneAs(dv[0]);
          x86::Vec s_vm = o.vm.cloneAs(dv[0]);

          pc->v_expand_alpha_16(xv, dv, kUseHi);
          pc->v_inv255_u16(xv, xv);
          pc->v_mul_u16(yv, dv, s_vn);
          pc->v_subs_u16(dv, dv, s_ux);
          pc->v_mul_u16(xv, xv, s_uy);
          pc->v_div255_u16(xv);
          pc->v_add_i16(dv, dv, xv);
          pc->v_mul_u16(dv, dv, s_vm);

          pc->v_add_i16(dv, dv, yv);
          pc->v_div255_u16(dv);
          out.uc.init(dv);
        }
        else {
          // Dca' = Clamp(Dca - Xca).m + Dca.(1 - m)
          // Da'  = <unchanged>
          dstFetch(d, n, PixelFlags::kUC, predicate);
          VecArray& dv = d.uc;

          x86::Vec s_ux = o.ux.cloneAs(dv[0]);
          x86::Vec s_vn = o.vn.cloneAs(dv[0]);
          x86::Vec s_vm = o.vm.cloneAs(dv[0]);

          pc->v_mul_u16(yv, dv, s_vn);
          pc->v_subs_u16(dv, dv, s_ux);
          pc->v_mul_u16(dv, dv, s_vm);

          pc->v_add_i16(dv, dv, yv);
          pc->v_div255_u16(dv);
          out.uc.init(dv);
        }
      }

      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Modulate
    // -----------------------------

    if (isModulate()) {
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);

      // Dca' = Dca.Xca
      // Da'  = Da .Xa
      pc->v_mul_u16(dv, dv, s_ux);
      pc->v_div255_u16(dv);

      if (!useDa)
        pc->vFillAlpha255W(dv, dv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Darken & Lighten
    // -------------------------------------

    if (isDarken() || isLighten()) {
      // Dca' = minmax(Dca + Xca.(1 - Da), Xca + Dca.Yca)
      // Da'  = Xa + Da.Ya
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);
      x86::Vec s_uy = o.uy.cloneAs(dv[0]);

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(xv, xv, s_ux);
      pc->v_div255_u16(xv);
      pc->v_add_i16(xv, xv, dv);
      pc->v_mul_u16(dv, dv, s_uy);
      pc->v_div255_u16(dv);
      pc->v_add_i16(dv, dv, s_ux);

      if (isDarken())
        pc->v_min_u8(dv, dv, xv);
      else
        pc->v_max_u8(dv, dv, xv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - LinearBurn
    // -------------------------------

    if (isLinearBurn()) {
      // Dca' = Dca + Xca - Yca.Da
      // Da'  = Da  + Xa  - Ya .Da
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);
      x86::Vec s_uy = o.uy.cloneAs(dv[0]);

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(xv, xv, s_uy);
      pc->v_add_i16(dv, dv, s_ux);
      pc->v_div255_u16(xv);
      pc->v_subs_u16(dv, dv, xv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Difference
    // -------------------------------

    if (isDifference()) {
      // Dca' = Dca + Sca - 2.min(Sca.Da, Dca.Sa)
      // Da'  = Da  + Sa  -   min(Sa .Da, Da .Sa)
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);
      x86::Vec s_uy = o.uy.cloneAs(dv[0]);

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(yv, s_uy, dv);
      pc->v_mul_u16(xv, xv, s_ux);
      pc->v_add_i16(dv, dv, s_ux);
      pc->v_min_u16(yv, yv, xv);
      pc->v_div255_u16(yv);
      pc->v_sub_i16(dv, dv, yv);
      pc->vZeroAlphaW(yv, yv);
      pc->v_sub_i16(dv, dv, yv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }

    // CMaskProc - RGBA32 - Exclusion
    // ------------------------------

    if (isExclusion()) {
      // Dca' = Dca + Xca - 2.Xca.Dca
      // Da'  = Da + Xa - Xa.Da
      dstFetch(d, n, PixelFlags::kUC, predicate);
      VecArray& dv = d.uc;

      x86::Vec s_ux = o.ux.cloneAs(dv[0]);

      pc->v_mul_u16(xv, dv, s_ux);
      pc->v_add_i16(dv, dv, s_ux);
      pc->v_div255_u16(xv);
      pc->v_sub_i16(dv, dv, xv);
      pc->vZeroAlphaW(xv, xv);
      pc->v_sub_i16(dv, dv, xv);

      out.uc.init(dv);
      pc->x_satisfy_pixel(out, flags);
      return;
    }
  }

  VecArray vm;
  if (_mask->vm.isValid()) {
    vm.init(_mask->vm);
  }

  vMaskProcRGBA32Vec(out, n, flags, vm, true, predicate);
}

// BLPipeline::JIT::CompOpPart - VMask - RGBA32 (Vec)
// ==================================================

void CompOpPart::vMaskProcRGBA32Vec(Pixel& out, PixelCount n, PixelFlags flags, VecArray& vm_, bool mImmutable, PixelPredicate& predicate) noexcept {
  SimdWidth simdWidth = pc->simdWidthOf(DataWidth::k64, n);
  uint32_t kFullN = pc->regCountOf(DataWidth::k64, n);
  uint32_t kUseHi = n > 1;
  uint32_t kSplit = kFullN == 1 ? 1 : 2;

  VecArray vm = vm_.cloneAs(simdWidth);
  bool hasMask = !vm.empty();

  bool useDa = hasDa();
  bool useSa = hasSa() || hasMask || isLoopCMask();

  VecArray xv, yv, zv;
  pc->newVecArray(xv, kFullN, simdWidth, "x");
  pc->newVecArray(yv, kFullN, simdWidth, "y");
  pc->newVecArray(zv, kFullN, simdWidth, "z");

  Pixel d("d", PixelType::kRGBA32);
  Pixel s("s", PixelType::kRGBA32);

  out.setCount(n);

  // VMaskProc - RGBA32 - SrcCopy
  // ----------------------------

  if (isSrcCopy()) {
    // Composition:
    //   Da - Optional.
    //   Sa - Optional.

    if (!hasMask) {
      // Dca' = Sca
      // Da'  = Sa
      srcFetch(out, n, flags, predicate);
    }
    else {
      // Dca' = Sca.m + Dca.(1 - m)
      // Da'  = Sa .m + Da .(1 - m)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& vs = s.uc;
      VecArray& vd = d.uc;
      VecArray vn;

      pc->v_mul_u16(vs, vs, vm);
      vMaskProcRGBA32InvertMask(vn, vm);

      pc->v_mul_u16(vd, vd, vn);
      pc->v_add_i16(vd, vd, vs);
      vMaskProcRGBA32InvertDone(vn, mImmutable);

      pc->v_div255_u16(vd);
      out.uc.init(vd);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - SrcOver
  // ----------------------------

  if (isSrcOver()) {
    // Composition:
    //   Da - Optional.
    //   Sa - Required, otherwise SRC_COPY.

    if (!hasMask) {
      // Dca' = Sca + Dca.(1 - Sa)
      // Da'  = Sa  + Da .(1 - Sa)
      srcFetch(s, n, PixelFlags::kPC | PixelFlags::kUI | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& uv = s.ui;
      VecArray& dv = d.uc;

      pc->v_mul_u16(dv, dv, uv);
      pc->v_div255_u16(dv);

      VecArray dh;
      if (pc->hasAVX()) {
        pc->_x_pack_pixel(dh, dv, n.value() * 4, "", "d");
      }
      else {
        dh = dv.even();
        pc->x_packs_i16_u8(dh, dh, dv.odd());
      }

      dh = dh.cloneAs(s.pc[0]);
      pc->v_add_i32(dh, dh, s.pc);

      out.pc.init(dh);
    }
    else {
      // Dca' = Sca.m + Dca.(1 - Sa.m)
      // Da'  = Sa .m + Da .(1 - Sa.m)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      pc->v_expand_alpha_16(xv, sv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(dv, dv, xv);
      pc->v_div255_u16(dv);

      pc->v_add_i16(dv, dv, sv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - SrcIn
  // --------------------------

  if (isSrcIn()) {
    // Composition:
    //   Da - Required, otherwise SRC_COPY.
    //   Sa - Optional.

    if (!hasMask) {
      // Dca' = Sca.Da
      // Da'  = Sa .Da
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUA, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.ua;

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Sca.m.Da + Dca.(1 - m)
      // Da'  = Sa .m.Da + Da .(1 - m)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(xv, xv, sv);
      pc->v_div255_u16(xv);
      pc->v_mul_u16(xv, xv, vm);
      vMaskProcRGBA32InvertMask(vm, vm);

      pc->v_mul_u16(dv, dv, vm);
      vMaskProcRGBA32InvertDone(vm, mImmutable);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - SrcOut
  // ---------------------------

  if (isSrcOut()) {
    // Composition:
    //   Da - Required, otherwise CLEAR.
    //   Sa - Optional.

    if (!hasMask) {
      // Dca' = Sca.(1 - Da)
      // Da'  = Sa .(1 - Da)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUI, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.ui;

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Sca.(1 - Da).m + Dca.(1 - m)
      // Da'  = Sa .(1 - Da).m + Da .(1 - m)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_inv255_u16(xv, xv);

      pc->v_mul_u16(xv, xv, sv);
      pc->v_div255_u16(xv);
      pc->v_mul_u16(xv, xv, vm);
      vMaskProcRGBA32InvertMask(vm, vm);

      pc->v_mul_u16(dv, dv, vm);
      vMaskProcRGBA32InvertDone(vm, mImmutable);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - SrcAtop
  // ----------------------------

  if (isSrcAtop()) {
    // Composition:
    //   Da - Required.
    //   Sa - Required.

    if (!hasMask) {
      // Dca' = Sca.Da + Dca.(1 - Sa)
      // Da'  = Sa .Da + Da .(1 - Sa) = Da
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kUI | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& uv = s.ui;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(dv, dv, uv);
      pc->v_mul_u16(xv, xv, sv);
      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);

      out.uc.init(dv);
    }
    else {
      // Dca' = Sca.Da.m + Dca.(1 - Sa.m)
      // Da'  = Sa .Da.m + Da .(1 - Sa.m) = Da
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      pc->v_expand_alpha_16(xv, sv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_expand_alpha_16(yv, dv, kUseHi);
      pc->v_mul_u16(dv, dv, xv);
      pc->v_mul_u16(yv, yv, sv);
      pc->v_add_i16(dv, dv, yv);
      pc->v_div255_u16(dv);

      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Dst
  // ------------------------

  if (isDstCopy()) {
    // Dca' = Dca
    // Da'  = Da
    BL_NOT_REACHED();
  }

  // VMaskProc - RGBA32 - DstOver
  // ----------------------------

  if (isDstOver()) {
    // Composition:
    //   Da - Required, otherwise DST_COPY.
    //   Sa - Optional.

    if (!hasMask) {
      // Dca' = Dca + Sca.(1 - Da)
      // Da'  = Da  + Sa .(1 - Da)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kPC | PixelFlags::kUI, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.ui;

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);

      VecArray dh;
      if (pc->hasAVX()) {
        pc->_x_pack_pixel(dh, dv, n.value() * 4, "", "d");
      }
      else {
        dh = dv.even();
        pc->x_packs_i16_u8(dh, dh, dv.odd());
      }

      dh = dh.cloneAs(d.pc[0]);
      pc->v_add_i32(dh, dh, d.pc);

      out.pc.init(dh);
    }
    else {
      // Dca' = Dca + Sca.m.(1 - Da)
      // Da'  = Da  + Sa .m.(1 - Da)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kPC | PixelFlags::kUI, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.ui;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);

      VecArray dh;
      if (pc->hasAVX()) {
        pc->_x_pack_pixel(dh, dv, n.value() * 4, "", "d");
      }
      else {
        dh = dv.even();
        pc->x_packs_i16_u8(dh, dh, dv.odd());
      }

      dh = dh.cloneAs(d.pc[0]);
      pc->v_add_i32(dh, dh, d.pc);

      out.pc.init(dh);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - DstIn
  // --------------------------

  if (isDstIn()) {
    // Composition:
    //   Da - Optional.
    //   Sa - Required, otherwise DST_COPY.

    if (!hasMask) {
      // Dca' = Dca.Sa
      // Da'  = Da .Sa
      srcFetch(s, n, PixelFlags::kUA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.ua;
      VecArray& dv = d.uc;

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Dca.(1 - m.(1 - Sa))
      // Da'  = Da .(1 - m.(1 - Sa))
      srcFetch(s, n, PixelFlags::kUI, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.ui;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      pc->v_inv255_u16(sv, sv);

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - DstOut
  // ---------------------------

  if (isDstOut()) {
    // Composition:
    //   Da - Optional.
    //   Sa - Required, otherwise CLEAR.

    if (!hasMask) {
      // Dca' = Dca.(1 - Sa)
      // Da'  = Da .(1 - Sa)
      srcFetch(s, n, PixelFlags::kUI | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.ui;
      VecArray& dv = d.uc;

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Dca.(1 - Sa.m)
      // Da'  = Da .(1 - Sa.m)
      srcFetch(s, n, PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.ua;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      pc->v_inv255_u16(sv, sv);

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    if (!useDa) pc->x_fill_pixel_alpha(out);
    return;
  }

  // VMaskProc - RGBA32 - DstAtop
  // ----------------------------

  if (isDstAtop()) {
    // Composition:
    //   Da - Required.
    //   Sa - Required.

    if (!hasMask) {
      // Dca' = Dca.Sa + Sca.(1 - Da)
      // Da'  = Da .Sa + Sa .(1 - Da)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kUA | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& uv = s.ua;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(dv, dv, uv);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(xv, xv, sv);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Dca.(1 - m.(1 - Sa)) + Sca.m.(1 - Da)
      // Da'  = Da .(1 - m.(1 - Sa)) + Sa .m.(1 - Da)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kUI, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& uv = s.ui;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(sv, sv, vm);
      pc->v_mul_u16(uv, uv, vm);

      pc->v_div255_u16(sv);
      pc->v_div255_u16(uv);
      pc->v_inv255_u16(xv, xv);
      pc->v_inv255_u16(uv, uv);
      pc->v_mul_u16(xv, xv, sv);
      pc->v_mul_u16(dv, dv, uv);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Xor
  // ------------------------

  if (isXor()) {
    // Composition:
    //   Da - Required.
    //   Sa - Required.

    if (!hasMask) {
      // Dca' = Dca.(1 - Sa) + Sca.(1 - Da)
      // Da'  = Da .(1 - Sa) + Sa .(1 - Da)
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kUI | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& uv = s.ui;
      VecArray& dv = d.uc;

      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_mul_u16(dv, dv, uv);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(xv, xv, sv);

      pc->v_add_i16(dv, dv, xv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }
    else {
      // Dca' = Dca.(1 - Sa.m) + Sca.m.(1 - Da)
      // Da'  = Da .(1 - Sa.m) + Sa .m.(1 - Da)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      pc->v_expand_alpha_16(xv, sv, kUseHi);
      pc->v_expand_alpha_16(yv, dv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_inv255_u16(yv, yv);
      pc->v_mul_u16(dv, dv, xv);
      pc->v_mul_u16(sv, sv, yv);

      pc->v_add_i16(dv, dv, sv);
      pc->v_div255_u16(dv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Plus
  // -------------------------

  if (isPlus()) {
    if (!hasMask) {
      // Dca' = Clamp(Dca + Sca)
      // Da'  = Clamp(Da  + Sa )
      srcFetch(s, n, PixelFlags::kPC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kPC, predicate);

      VecArray& sh = s.pc;
      VecArray& dh = d.pc;

      pc->v_adds_u8(dh, dh, sh);
      out.pc.init(dh);
    }
    else {
      // Dca' = Clamp(Dca + Sca.m)
      // Da'  = Clamp(Da  + Sa .m)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kPC, predicate);

      VecArray& sv = s.uc;
      VecArray& dh = d.pc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      VecArray sh;
      if (pc->hasAVX()) {
        pc->_x_pack_pixel(sh, sv, n.value() * 4, "", "s");
      }
      else {
        sh = sv.even();
        pc->x_packs_i16_u8(sh, sh, sv.odd());
      }

      pc->v_adds_u8(dh, dh, sh.cloneAs(dh[0]));

      out.pc.init(dh);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Minus
  // --------------------------

  if (isMinus()) {
    if (!hasMask) {
      // Dca' = Clamp(Dca - Sca) + Sca.(1 - Da)
      // Da'  = Da + Sa.(1 - Da)
      if (useDa) {
        srcFetch(s, n, PixelFlags::kUC, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_expand_alpha_16(xv, dv, kUseHi);
        pc->v_inv255_u16(xv, xv);
        pc->v_mul_u16(xv, xv, sv);
        pc->vZeroAlphaW(sv, sv);
        pc->v_div255_u16(xv);

        pc->v_subs_u16(dv, dv, sv);
        pc->v_add_i16(dv, dv, xv);
        out.uc.init(dv);
      }
      // Dca' = Clamp(Dca - Sca)
      // Da'  = <unchanged>
      else {
        srcFetch(s, n, PixelFlags::kPC, predicate);
        dstFetch(d, n, PixelFlags::kPC, predicate);

        VecArray& sh = s.pc;
        VecArray& dh = d.pc;

        pc->vZeroAlphaB(sh, sh);
        pc->v_subs_u8(dh, dh, sh);

        out.pc.init(dh);
      }
    }
    else {
      // Dca' = (Clamp(Dca - Sca) + Sca.(1 - Da)).m + Dca.(1 - m)
      // Da'  = Da + Sa.m(1 - Da)
      if (useDa) {
        srcFetch(s, n, PixelFlags::kUC, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_expand_alpha_16(xv, dv, kUseHi);
        pc->v_mov(yv, dv);
        pc->v_inv255_u16(xv, xv);
        pc->v_subs_u16(dv, dv, sv);
        pc->v_mul_u16(sv, sv, xv);

        pc->vZeroAlphaW(dv, dv);
        pc->v_div255_u16(sv);
        pc->v_add_i16(dv, dv, sv);
        pc->v_mul_u16(dv, dv, vm);

        pc->vZeroAlphaW(vm, vm);
        pc->v_inv255_u16(vm, vm);

        pc->v_mul_u16(yv, yv, vm);

        if (mImmutable) {
          pc->v_inv255_u16(vm[0], vm[0]);
          pc->v_swizzle_u32(vm[0], vm[0], x86::shuffleImm(2, 2, 0, 0));
        }

        pc->v_add_i16(dv, dv, yv);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      // Dca' = Clamp(Dca - Sca).m + Dca.(1 - m)
      // Da'  = <unchanged>
      else {
        srcFetch(s, n, PixelFlags::kUC, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_inv255_u16(xv, vm);
        pc->vZeroAlphaW(sv, sv);

        pc->v_mul_u16(xv, xv, dv);
        pc->v_subs_u16(dv, dv, sv);
        pc->v_mul_u16(dv, dv, vm);

        pc->v_add_i16(dv, dv, xv);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Modulate
  // -----------------------------

  if (isModulate()) {
    VecArray& dv = d.uc;
    VecArray& sv = s.uc;

    if (!hasMask) {
      // Dca' = Dca.Sca
      // Da'  = Da .Sa
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);
    }
    else {
      // Dca' = Dca.(Sca.m + 1 - m)
      // Da'  = Da .(Sa .m + 1 - m)
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      pc->v_add_i16(sv, sv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, sv));
      pc->v_sub_i16(sv, sv, vm);
      pc->v_mul_u16(dv, dv, sv);
      pc->v_div255_u16(dv);

      out.uc.init(dv);
    }

    if (!useDa)
      pc->vFillAlpha255W(dv, dv);

    out.uc.init(dv);
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Multiply
  // -----------------------------

  if (isMultiply()) {
    if (!hasMask) {
      if (useDa && useSa) {
        // Dca' = Dca.(Sca + 1 - Sa) + Sca.(1 - Da)
        // Da'  = Da .(Sa  + 1 - Sa) + Sa .(1 - Da)
        srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        // SPLIT.
        for (unsigned int i = 0; i < kSplit; i++) {
          VecArray sh = sv.even_odd(i);
          VecArray dh = dv.even_odd(i);
          VecArray xh = xv.even_odd(i);
          VecArray yh = yv.even_odd(i);

          pc->v_expand_alpha_16(yh, sh, kUseHi);
          pc->v_expand_alpha_16(xh, dh, kUseHi);
          pc->v_inv255_u16(yh, yh);
          pc->v_add_i16(yh, yh, sh);
          pc->v_inv255_u16(xh, xh);
          pc->v_mul_u16(dh, dh, yh);
          pc->v_mul_u16(xh, xh, sh);
          pc->v_add_i16(dh, dh, xh);
        }

        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      else if (useDa) {
        // Dca' = Sc.(Dca + 1 - Da)
        // Da'  = 1 .(Da  + 1 - Da) = 1
        srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_expand_alpha_16(xv, dv, kUseHi);
        pc->v_inv255_u16(xv, xv);
        pc->v_add_i16(dv, dv, xv);
        pc->v_mul_u16(dv, dv, sv);

        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      else if (hasSa()) {
        // Dc'  = Dc.(Sca + 1 - Sa)
        // Da'  = Da.(Sa  + 1 - Sa)
        srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_expand_alpha_16(xv, sv, kUseHi);
        pc->v_inv255_u16(xv, xv);
        pc->v_add_i16(xv, xv, sv);
        pc->v_mul_u16(dv, dv, xv);

        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      else {
        // Dc' = Dc.Sc
        srcFetch(s, n, PixelFlags::kUC | PixelFlags::kImmutable, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_mul_u16(dv, dv, sv);
        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
    }
    else {
      if (useDa) {
        // Dca' = Dca.(Sca.m + 1 - Sa.m) + Sca.m(1 - Da)
        // Da'  = Da .(Sa .m + 1 - Sa.m) + Sa .m(1 - Da)
        srcFetch(s, n, PixelFlags::kUC, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_mul_u16(sv, sv, vm);
        pc->v_div255_u16(sv);

        // SPLIT.
        for (unsigned int i = 0; i < kSplit; i++) {
          VecArray sh = sv.even_odd(i);
          VecArray dh = dv.even_odd(i);
          VecArray xh = xv.even_odd(i);
          VecArray yh = yv.even_odd(i);

          pc->v_expand_alpha_16(yh, sh, kUseHi);
          pc->v_expand_alpha_16(xh, dh, kUseHi);
          pc->v_inv255_u16(yh, yh);
          pc->v_add_i16(yh, yh, sh);
          pc->v_inv255_u16(xh, xh);
          pc->v_mul_u16(dh, dh, yh);
          pc->v_mul_u16(xh, xh, sh);
          pc->v_add_i16(dh, dh, xh);
        }

        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
      else {
        srcFetch(s, n, PixelFlags::kUC, predicate);
        dstFetch(d, n, PixelFlags::kUC, predicate);

        VecArray& sv = s.uc;
        VecArray& dv = d.uc;

        pc->v_mul_u16(sv, sv, vm);
        pc->v_div255_u16(sv);

        pc->v_expand_alpha_16(xv, sv, kUseHi);
        pc->v_inv255_u16(xv, xv);
        pc->v_add_i16(xv, xv, sv);
        pc->v_mul_u16(dv, dv, xv);

        pc->v_div255_u16(dv);
        out.uc.init(dv);
      }
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Overlay
  // ----------------------------

  if (isOverlay()) {
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      useSa = true;
    }

    if (useSa) {
      // if (2.Dca < Da)
      //   Dca' = Dca + Sca - (Dca.Sa + Sca.Da - 2.Sca.Dca)
      //   Da'  = Da  + Sa  - (Da .Sa + Sa .Da - 2.Sa .Da ) - Sa.Da
      //   Da'  = Da  + Sa  - Sa.Da
      // else
      //   Dca' = Dca + Sca + (Dca.Sa + Sca.Da - 2.Sca.Dca) - Sa.Da
      //   Da'  = Da  + Sa  + (Da .Sa + Sa .Da - 2.Sa .Da ) - Sa.Da
      //   Da'  = Da  + Sa  - Sa.Da

      for (unsigned int i = 0; i < kSplit; i++) {
        VecArray sh = sv.even_odd(i);
        VecArray dh = dv.even_odd(i);

        VecArray xh = xv.even_odd(i);
        VecArray yh = yv.even_odd(i);
        VecArray zh = zv.even_odd(i);

        if (!useDa)
          pc->vFillAlpha255W(dh, dh);

        pc->v_expand_alpha_16(xh, dh, kUseHi);
        pc->v_expand_alpha_16(yh, sh, kUseHi);

        pc->v_mul_u16(xh, xh, sh);                                 // Sca.Da
        pc->v_mul_u16(yh, yh, dh);                                 // Dca.Sa
        pc->v_mul_u16(zh, dh, sh);                                 // Dca.Sca

        pc->v_add_i16(sh, sh, dh);                                 // Dca + Sca
        pc->v_sub_i16(xh, xh, zh);                                 // Sca.Da - Dca.Sca
        pc->vZeroAlphaW(zh, zh);
        pc->v_add_i16(xh, xh, yh);                                 // Dca.Sa + Sca.Da - Dca.Sca
        pc->v_expand_alpha_16(yh, dh, kUseHi);                        // Da
        pc->v_sub_i16(xh, xh, zh);                                 // [C=Dca.Sa + Sca.Da - 2.Dca.Sca] [A=Sa.Da]

        pc->v_sll_i16(dh, dh, 1);                                  // 2.Dca
        pc->v_cmp_gt_i16(yh, yh, dh);                              // 2.Dca < Da
        pc->v_div255_u16(xh);
        pc->v_or_i64(yh, yh, pc->simdConst(&ct.i_FFFF000000000000, Bcst::k64, yh));

        pc->v_expand_alpha_16(zh, xh, kUseHi);
        // if (2.Dca < Da)
        //   X = [C = -(Dca.Sa + Sca.Da - 2.Sca.Dca)] [A = -Sa.Da]
        // else
        //   X = [C =  (Dca.Sa + Sca.Da - 2.Sca.Dca)] [A = -Sa.Da]
        pc->v_xor_i32(xh, xh, yh);
        pc->v_sub_i16(xh, xh, yh);

        // if (2.Dca < Da)
        //   Y = [C = 0] [A = 0]
        // else
        //   Y = [C = Sa.Da] [A = 0]
        pc->v_nand_i32(yh, yh, zh);

        pc->v_add_i16(sh, sh, xh);
        pc->v_sub_i16(sh, sh, yh);
      }

      out.uc.init(sv);
    }
    else if (useDa) {
      // if (2.Dca < Da)
      //   Dca' = Sc.(1 + 2.Dca - Da)
      //   Da'  = 1
      // else
      //   Dca' = 2.Dca - Da + Sc.(1 - (2.Dca - Da))
      //   Da'  = 1

      pc->v_expand_alpha_16(xv, dv, kUseHi);                          // Da
      pc->v_sll_i16(dv, dv, 1);                                    // 2.Dca

      pc->v_cmp_gt_i16(yv, xv, dv);                                //  (2.Dca < Da) ? -1 : 0
      pc->v_sub_i16(xv, xv, dv);                                   // -(2.Dca - Da)

      pc->v_xor_i32(xv, xv, yv);
      pc->v_sub_i16(xv, xv, yv);                                   // 2.Dca < Da ? 2.Dca - Da : -(2.Dca - Da)
      pc->v_nand_i32(yv, yv, xv);                                  // 2.Dca < Da ? 0          : -(2.Dca - Da)
      pc->v_add_i16(xv, xv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, xv));

      pc->v_mul_u16(xv, xv, sv);
      pc->v_div255_u16(xv);
      pc->v_sub_i16(xv, xv, yv);

      out.uc.init(xv);
    }
    else {
      // if (2.Dc < 1)
      //   Dc'  = 2.Dc.Sc
      // else
      //   Dc'  = 2.Dc + 2.Sc - 1 - 2.Dc.Sc

      pc->v_mul_u16(xv, dv, sv);                                                                 // Dc.Sc
      pc->v_cmp_gt_i16(yv, dv, pc->simdConst(&ct.i_007F007F007F007F, Bcst::kNA, yv)); // !(2.Dc < 1)
      pc->v_add_i16(dv, dv, sv);                                                                 // Dc + Sc
      pc->v_div255_u16(xv);

      pc->v_sll_i16(dv, dv, 1);                                                                  // 2.Dc + 2.Sc
      pc->v_sll_i16(xv, xv, 1);                                                                  // 2.Dc.Sc
      pc->v_sub_i16(dv, dv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, dv));    // 2.Dc + 2.Sc - 1

      pc->v_xor_i32(xv, xv, yv);
      pc->v_and_i32(dv, dv, yv);                                                                 // 2.Dc < 1 ? 0 : 2.Dc + 2.Sc - 1
      pc->v_sub_i16(xv, xv, yv);                                                                 // 2.Dc < 1 ? 2.Dc.Sc : -2.Dc.Sc
      pc->v_add_i16(dv, dv, xv);                                                                 // 2.Dc < 1 ? 2.Dc.Sc : 2.Dc + 2.Sc - 1 - 2.Dc.Sc

      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Screen
  // ---------------------------

  if (isScreen()) {
    // Dca' = Sca + Dca.(1 - Sca)
    // Da'  = Sa  + Da .(1 - Sa)
    srcFetch(s, n, PixelFlags::kUC | (hasMask ? PixelFlags::kNone : PixelFlags::kImmutable), predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
    }

    pc->v_inv255_u16(xv, sv);
    pc->v_mul_u16(dv, dv, xv);
    pc->v_div255_u16(dv);
    pc->v_add_i16(dv, dv, sv);

    out.uc.init(dv);
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Darken & Lighten
  // -------------------------------------

  if (isDarken() || isLighten()) {
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    bool minMaxPredicate = isDarken();

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      useSa = true;
    }

    if (useSa && useDa) {
      // Dca' = minmax(Dca + Sca.(1 - Da), Sca + Dca.(1 - Sa))
      // Da'  = Sa + Da.(1 - Sa)
      for (unsigned int i = 0; i < kSplit; i++) {
        VecArray sh = sv.even_odd(i);
        VecArray dh = dv.even_odd(i);
        VecArray xh = xv.even_odd(i);
        VecArray yh = yv.even_odd(i);

        pc->v_expand_alpha_16(xh, dh, kUseHi);
        pc->v_expand_alpha_16(yh, sh, kUseHi);

        pc->v_inv255_u16(xh, xh);
        pc->v_inv255_u16(yh, yh);

        pc->v_mul_u16(xh, xh, sh);
        pc->v_mul_u16(yh, yh, dh);
        pc->v_div255_u16_2x(xh, yh);

        pc->v_add_i16(dh, dh, xh);
        pc->v_add_i16(sh, sh, yh);

        pc->v_min_or_max_u8(dh, dh, sh, minMaxPredicate);
      }

      out.uc.init(dv);
    }
    else if (useDa) {
      // Dca' = minmax(Dca + Sc.(1 - Da), Sc)
      // Da'  = 1
      pc->v_expand_alpha_16(xv, dv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(xv, xv, sv);
      pc->v_div255_u16(xv);
      pc->v_add_i16(dv, dv, xv);
      pc->v_min_or_max_u8(dv, dv, sv, minMaxPredicate);

      out.uc.init(dv);
    }
    else if (useSa) {
      // Dc' = minmax(Dc, Sca + Dc.(1 - Sa))
      pc->v_expand_alpha_16(xv, sv, kUseHi);
      pc->v_inv255_u16(xv, xv);
      pc->v_mul_u16(xv, xv, dv);
      pc->v_div255_u16(xv);
      pc->v_add_i16(xv, xv, sv);
      pc->v_min_or_max_u8(dv, dv, xv, minMaxPredicate);

      out.uc.init(dv);
    }
    else {
      // Dc' = minmax(Dc, Sc)
      pc->v_min_or_max_u8(dv, dv, sv, minMaxPredicate);

      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - ColorDodge (SCALAR)
  // ----------------------------------------

  if (isColorDodge() && n == 1) {
    // Dca' = min(Dca.Sa.Sa / max(Sa - Sca, 0.001), Sa.Da) + Sca.(1 - Da) + Dca.(1 - Sa);
    // Da'  = min(Da .Sa.Sa / max(Sa - Sa , 0.001), Sa.Da) + Sa .(1 - Da) + Da .(1 - Sa);

    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kPC, predicate);

    x86::Vec& s0 = s.uc[0];
    x86::Vec& d0 = d.pc[0];
    x86::Vec& x0 = xv[0];
    x86::Vec& y0 = yv[0];
    x86::Vec& z0 = zv[0];

    if (hasMask) {
      pc->v_mul_u16(s0, s0, vm[0]);
      pc->v_div255_u16(s0);
    }

    pc->v_mov_u8_u32(d0, d0);
    pc->v_mov_u16_u32(s0, s0);

    pc->v_cvt_i32_f32(y0, s0);
    pc->v_cvt_i32_f32(z0, d0);
    pc->v_packs_i32_i16(d0, d0, s0);

    pc->vExpandAlphaPS(x0, y0);
    pc->v_xor_f32(y0, y0, pc->simdConst(&ct.f32_sgn, Bcst::k32, y0));
    pc->v_mul_f32(z0, z0, x0);
    pc->v_and_f32(y0, y0, pc->simdConst(&ct.i_FFFFFFFF_FFFFFFFF_FFFFFFFF_0, Bcst::kNA, y0));
    pc->v_add_f32(y0, y0, x0);

    pc->v_max_f32(y0, y0, pc->simdConst(&ct.f32_1e_m3, Bcst::k32, y0));
    pc->v_div_f32(z0, z0, y0);

    pc->v_swizzle_u32(s0, d0, x86::shuffleImm(1, 1, 3, 3));
    pc->vExpandAlphaHi16(s0, s0);
    pc->vExpandAlphaLo16(s0, s0);
    pc->v_inv255_u16(s0, s0);
    pc->v_mul_u16(d0, d0, s0);
    pc->v_swizzle_u32(s0, d0, x86::shuffleImm(1, 0, 3, 2));
    pc->v_add_i16(d0, d0, s0);

    pc->v_mul_f32(z0, z0, x0);
    pc->vExpandAlphaPS(x0, z0);
    pc->v_min_f32(z0, z0, x0);

    pc->v_cvtt_f32_i32(z0, z0);
    pc->xPackU32ToU16Lo(z0, z0);
    pc->v_add_i16(d0, d0, z0);

    pc->v_div255_u16(d0);
    out.uc.init(d0);

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - ColorBurn (SCALAR)
  // ---------------------------------------

  if (isColorBurn() && n == 1) {
    // Dca' = Sa.Da - min(Sa.Da, (Da - Dca).Sa.Sa / max(Sca, 0.001)) + Sca.(1 - Da) + Dca.(1 - Sa)
    // Da'  = Sa.Da - min(Sa.Da, (Da - Da ).Sa.Sa / max(Sa , 0.001)) + Sa .(1 - Da) + Da .(1 - Sa)
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kPC, predicate);

    x86::Vec& s0 = s.uc[0];
    x86::Vec& d0 = d.pc[0];
    x86::Vec& x0 = xv[0];
    x86::Vec& y0 = yv[0];
    x86::Vec& z0 = zv[0];

    if (hasMask) {
      pc->v_mul_u16(s0, s0, vm[0]);
      pc->v_div255_u16(s0);
    }

    pc->v_mov_u8_u32(d0, d0);
    pc->v_mov_u16_u32(s0, s0);

    pc->v_cvt_i32_f32(y0, s0);
    pc->v_cvt_i32_f32(z0, d0);
    pc->v_packs_i32_i16(d0, d0, s0);

    pc->vExpandAlphaPS(x0, y0);
    pc->v_max_f32(y0, y0, pc->simdConst(&ct.f32_1e_m3, Bcst::k32, y0));
    pc->v_mul_f32(z0, z0, x0);                                     // Dca.Sa

    pc->vExpandAlphaPS(x0, z0);                                    // Sa.Da
    pc->v_xor_f32(z0, z0, pc->simdConst(&ct.f32_sgn, Bcst::k32, z0));

    pc->v_and_f32(z0, z0, pc->simdConst(&ct.i_FFFFFFFF_FFFFFFFF_FFFFFFFF_0, Bcst::kNA, z0));
    pc->v_add_f32(z0, z0, x0);                                     // (Da - Dxa).Sa
    pc->v_div_f32(z0, z0, y0);

    pc->v_swizzle_u32(s0, d0, x86::shuffleImm(1, 1, 3, 3));
    pc->vExpandAlphaHi16(s0, s0);
    pc->vExpandAlphaLo16(s0, s0);
    pc->v_inv255_u16(s0, s0);
    pc->v_mul_u16(d0, d0, s0);
    pc->v_swizzle_u32(s0, d0, x86::shuffleImm(1, 0, 3, 2));
    pc->v_add_i16(d0, d0, s0);

    pc->vExpandAlphaPS(x0, y0);                                    // Sa
    pc->v_mul_f32(z0, z0, x0);
    pc->vExpandAlphaPS(x0, z0);                                    // Sa.Da
    pc->v_min_f32(z0, z0, x0);
    pc->v_and_f32(z0, z0, pc->simdConst(&ct.i_FFFFFFFF_FFFFFFFF_FFFFFFFF_0, Bcst::kNA, z0));
    pc->v_sub_f32(x0, x0, z0);

    pc->v_cvtt_f32_i32(x0, x0);
    pc->xPackU32ToU16Lo(x0, x0);
    pc->v_add_i16(d0, d0, x0);

    pc->v_div255_u16(d0);
    out.uc.init(d0);

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - LinearBurn
  // -------------------------------

  if (isLinearBurn()) {
    srcFetch(s, n, PixelFlags::kUC | (hasMask ? PixelFlags::kNone : PixelFlags::kImmutable), predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
    }

    if (useDa && useSa) {
      // Dca' = Dca + Sca - Sa.Da
      // Da'  = Da  + Sa  - Sa.Da
      pc->v_expand_alpha_16(xv, sv, kUseHi);
      pc->v_expand_alpha_16(yv, dv, kUseHi);
      pc->v_mul_u16(xv, xv, yv);
      pc->v_div255_u16(xv);
      pc->v_add_i16(dv, dv, sv);
      pc->v_subs_u16(dv, dv, xv);
    }
    else if (useDa || useSa) {
      pc->v_expand_alpha_16(xv, useDa ? dv : sv, kUseHi);
      pc->v_add_i16(dv, dv, sv);
      pc->v_subs_u16(dv, dv, xv);
    }
    else {
      // Dca' = Dc + Sc - 1
      pc->v_add_i16(dv, dv, sv);
      pc->v_subs_u16(dv, dv, pc->simdConst(&ct.i_000000FF00FF00FF, Bcst::kNA, dv));
    }

    out.uc.init(dv);
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - LinearLight
  // --------------------------------

  if (isLinearLight() && n == 1) {
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
      useSa = 1;
    }

    if (useSa || useDa) {
      // Dca' = min(max((Dca.Sa + 2.Sca.Da - Sa.Da), 0), Sa.Da) + Sca.(1 - Da) + Dca.(1 - Sa)
      // Da'  = min(max((Da .Sa + 2.Sa .Da - Sa.Da), 0), Sa.Da) + Sa .(1 - Da) + Da .(1 - Sa)

      x86::Vec& d0 = dv[0];
      x86::Vec& s0 = sv[0];
      x86::Vec& x0 = xv[0];
      x86::Vec& y0 = yv[0];

      pc->vExpandAlphaLo16(y0, d0);
      pc->vExpandAlphaLo16(x0, s0);

      pc->v_interleave_lo_u64(d0, d0, s0);
      pc->v_interleave_lo_u64(x0, x0, y0);

      pc->v_mov(s0, d0);
      pc->v_mul_u16(d0, d0, x0);
      pc->v_inv255_u16(x0, x0);
      pc->v_div255_u16(d0);

      pc->v_mul_u16(s0, s0, x0);
      pc->v_swap_u64(x0, s0);
      pc->v_swap_u64(y0, d0);
      pc->v_add_i16(s0, s0, x0);
      pc->v_add_i16(d0, d0, y0);
      pc->vExpandAlphaLo16(x0, y0);
      pc->v_add_i16(d0, d0, y0);
      pc->v_div255_u16(s0);

      pc->v_subs_u16(d0, d0, x0);
      pc->v_min_i16(d0, d0, x0);

      pc->v_add_i16(d0, d0, s0);
      out.uc.init(d0);
    }
    else {
      // Dc' = min(max((Dc + 2.Sc - 1), 0), 1)
      pc->v_sll_i16(sv, sv, 1);
      pc->v_add_i16(dv, dv, sv);
      pc->v_subs_u16(dv, dv, pc->simdConst(&ct.i_000000FF00FF00FF, Bcst::kNA, dv));
      pc->v_min_i16(dv, dv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, dv));

      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - PinLight
  // -----------------------------

  if (isPinLight()) {
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      useSa = true;
    }

    if (useSa && useDa) {
      // if 2.Sca <= Sa
      //   Dca' = min(Dca + Sca - Sca.Da, Dca + Sca + Sca.Da - Dca.Sa)
      //   Da'  = min(Da  + Sa  - Sa .Da, Da  + Sa  + Sa .Da - Da .Sa) = Da + Sa.(1 - Da)
      // else
      //   Dca' = max(Dca + Sca - Sca.Da, Dca + Sca + Sca.Da - Dca.Sa - Da.Sa)
      //   Da'  = max(Da  + Sa  - Sa .Da, Da  + Sa  + Sa .Da - Da .Sa - Da.Sa) = Da + Sa.(1 - Da)

      pc->v_expand_alpha_16(yv, sv, kUseHi);                                                          // Sa
      pc->v_expand_alpha_16(xv, dv, kUseHi);                                                          // Da

      pc->v_mul_u16(yv, yv, dv);                                                                   // Dca.Sa
      pc->v_mul_u16(xv, xv, sv);                                                                   // Sca.Da
      pc->v_add_i16(dv, dv, sv);                                                                   // Dca + Sca
      pc->v_div255_u16_2x(yv, xv);

      pc->v_sub_i16(yv, yv, dv);                                                                   // Dca.Sa - Dca - Sca
      pc->v_sub_i16(dv, dv, xv);                                                                   // Dca + Sca - Sca.Da
      pc->v_sub_i16(xv, xv, yv);                                                                   // Dca + Sca + Sca.Da - Dca.Sa

      pc->v_expand_alpha_16(yv, sv, kUseHi);                                                          // Sa
      pc->v_sll_i16(sv, sv, 1);                                                                    // 2.Sca
      pc->v_cmp_gt_i16(sv, sv, yv);                                                                // !(2.Sca <= Sa)

      pc->v_sub_i16(zv, dv, xv);
      pc->v_expand_alpha_16(zv, zv, kUseHi);                                                          // -Da.Sa
      pc->v_and_i32(zv, zv, sv);                                                                   // 2.Sca <= Sa ? 0 : -Da.Sa
      pc->v_add_i16(xv, xv, zv);                                                                   // 2.Sca <= Sa ? Dca + Sca + Sca.Da - Dca.Sa : Dca + Sca + Sca.Da - Dca.Sa - Da.Sa

      // if 2.Sca <= Sa:
      //   min(dv, xv)
      // else
      //   max(dv, xv) <- ~min(~dv, ~xv)
      pc->v_xor_i32(dv, dv, sv);
      pc->v_xor_i32(xv, xv, sv);
      pc->v_min_i16(dv, dv, xv);
      pc->v_xor_i32(dv, dv, sv);

      out.uc.init(dv);
    }
    else if (useDa) {
      // if 2.Sc <= 1
      //   Dca' = min(Dca + Sc - Sc.Da, Sc + Sc.Da)
      //   Da'  = min(Da  + 1  - 1 .Da, 1  + 1 .Da) = 1
      // else
      //   Dca' = max(Dca + Sc - Sc.Da, Sc + Sc.Da - Da)
      //   Da'  = max(Da  + 1  - 1 .Da, 1  + 1 .Da - Da) = 1

      pc->v_expand_alpha_16(xv, dv, kUseHi);                                                          // Da
      pc->v_mul_u16(xv, xv, sv);                                                                   // Sc.Da
      pc->v_add_i16(dv, dv, sv);                                                                   // Dca + Sc
      pc->v_div255_u16(xv);

      pc->v_cmp_gt_i16(yv, sv, pc->simdConst(&ct.i_007F007F007F007F, Bcst::kNA, yv));               // !(2.Sc <= 1)
      pc->v_add_i16(sv, sv, xv);                                                                   // Sc + Sc.Da
      pc->v_sub_i16(dv, dv, xv);                                                                   // Dca + Sc - Sc.Da
      pc->v_expand_alpha_16(xv, xv);                                                                  // Da
      pc->v_and_i32(xv, xv, yv);                                                                   // 2.Sc <= 1 ? 0 : Da
      pc->v_sub_i16(sv, sv, xv);                                                                   // 2.Sc <= 1 ? Sc + Sc.Da : Sc + Sc.Da - Da

      // if 2.Sc <= 1:
      //   min(dv, sv)
      // else
      //   max(dv, sv) <- ~min(~dv, ~sv)
      pc->v_xor_i32(dv, dv, yv);
      pc->v_xor_i32(sv, sv, yv);
      pc->v_min_i16(dv, dv, sv);
      pc->v_xor_i32(dv, dv, yv);

      out.uc.init(dv);
    }
    else if (useSa) {
      // if 2.Sca <= Sa
      //   Dc' = min(Dc, Dc + 2.Sca - Dc.Sa)
      // else
      //   Dc' = max(Dc, Dc + 2.Sca - Dc.Sa - Sa)

      pc->v_expand_alpha_16(xv, sv, kUseHi);                                                          // Sa
      pc->v_sll_i16(sv, sv, 1);                                                                    // 2.Sca
      pc->v_cmp_gt_i16(yv, sv, xv);                                                                // !(2.Sca <= Sa)
      pc->v_and_i32(yv, yv, xv);                                                                   // 2.Sca <= Sa ? 0 : Sa
      pc->v_mul_u16(xv, xv, dv);                                                                   // Dc.Sa
      pc->v_add_i16(sv, sv, dv);                                                                   // Dc + 2.Sca
      pc->v_div255_u16(xv);
      pc->v_sub_i16(sv, sv, yv);                                                                   // 2.Sca <= Sa ? Dc + 2.Sca : Dc + 2.Sca - Sa
      pc->v_cmp_eq_i16(yv, yv, pc->simdConst(&ct.i_0000000000000000, Bcst::kNA, yv));               // 2.Sc <= 1
      pc->v_sub_i16(sv, sv, xv);                                                                   // 2.Sca <= Sa ? Dc + 2.Sca - Dc.Sa : Dc + 2.Sca - Dc.Sa - Sa

      // if 2.Sc <= 1:
      //   min(dv, sv)
      // else
      //   max(dv, sv) <- ~min(~dv, ~sv)
      pc->v_xor_i32(dv, dv, yv);
      pc->v_xor_i32(sv, sv, yv);
      pc->v_max_i16(dv, dv, sv);
      pc->v_xor_i32(dv, dv, yv);

      out.uc.init(dv);
    }
    else {
      // if 2.Sc <= 1
      //   Dc' = min(Dc, 2.Sc)
      // else
      //   Dc' = max(Dc, 2.Sc - 1)

      pc->v_sll_i16(sv, sv, 1);                                                                    // 2.Sc
      pc->v_min_i16(xv, sv, dv);                                                                   // min(Dc, 2.Sc)

      pc->v_cmp_gt_i16(yv, sv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, yv));               // !(2.Sc <= 1)
      pc->v_sub_i16(sv, sv, pc->simdConst(&ct.i_00FF00FF00FF00FF, Bcst::kNA, sv));                  // 2.Sc - 1
      pc->v_max_i16(dv, dv, sv);                                                                   // max(Dc, 2.Sc - 1)

      pc->v_blendv_u8_destructive(xv, xv, dv, yv);                                                 // 2.Sc <= 1 ? min(Dc, 2.Sc) : max(Dc, 2.Sc - 1)
      out.uc.init(xv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - HardLight
  // ------------------------------

  if (isHardLight()) {
    // if (2.Sca < Sa)
    //   Dca' = Dca + Sca - (Dca.Sa + Sca.Da - 2.Sca.Dca)
    //   Da'  = Da  + Sa  - Sa.Da
    // else
    //   Dca' = Dca + Sca + (Dca.Sa + Sca.Da - 2.Sca.Dca) - Sa.Da
    //   Da'  = Da  + Sa  - Sa.Da
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
    }

    // SPLIT.
    for (unsigned int i = 0; i < kSplit; i++) {
      VecArray sh = sv.even_odd(i);
      VecArray dh = dv.even_odd(i);
      VecArray xh = xv.even_odd(i);
      VecArray yh = yv.even_odd(i);
      VecArray zh = zv.even_odd(i);

      pc->v_expand_alpha_16(xh, dh, kUseHi);
      pc->v_expand_alpha_16(yh, sh, kUseHi);

      pc->v_mul_u16(xh, xh, sh); // Sca.Da
      pc->v_mul_u16(yh, yh, dh); // Dca.Sa
      pc->v_mul_u16(zh, dh, sh); // Dca.Sca

      pc->v_add_i16(dh, dh, sh);
      pc->v_sub_i16(xh, xh, zh);
      pc->v_add_i16(xh, xh, yh);
      pc->v_sub_i16(xh, xh, zh);

      pc->v_expand_alpha_16(yh, yh, kUseHi);
      pc->v_expand_alpha_16(zh, sh, kUseHi);
      pc->v_div255_u16_2x(xh, yh);

      pc->v_sll_i16(sh, sh, 1);
      pc->v_cmp_gt_i16(zh, zh, sh);

      pc->v_xor_i32(xh, xh, zh);
      pc->v_sub_i16(xh, xh, zh);
      pc->vZeroAlphaW(zh, zh);
      pc->v_nand_i32(zh, zh, yh);
      pc->v_add_i16(dh, dh, xh);
      pc->v_sub_i16(dh, dh, zh);
    }

    out.uc.init(dv);
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - SoftLight (SCALAR)
  // ---------------------------------------

  if (isSoftLight() && n == 1) {
    // Dc = Dca/Da
    //
    // Dca' =
    //   if 2.Sca - Sa <= 0
    //     Dca + Sca.(1 - Da) + (2.Sca - Sa).Da.[[              Dc.(1 - Dc)           ]]
    //   else if 2.Sca - Sa > 0 and 4.Dc <= 1
    //     Dca + Sca.(1 - Da) + (2.Sca - Sa).Da.[[ 4.Dc.(4.Dc.Dc + Dc - 4.Dc + 1) - Dc]]
    //   else
    //     Dca + Sca.(1 - Da) + (2.Sca - Sa).Da.[[             sqrt(Dc) - Dc          ]]
    // Da'  = Da + Sa - Sa.Da
    srcFetch(s, n, PixelFlags::kUC, predicate);
    dstFetch(d, n, PixelFlags::kPC, predicate);

    x86::Vec& s0 = s.uc[0];
    x86::Vec& d0 = d.pc[0];

    x86::Vec  a0 = cc->newXmm("a0");
    x86::Vec  b0 = cc->newXmm("b0");
    x86::Vec& x0 = xv[0];
    x86::Vec& y0 = yv[0];
    x86::Vec& z0 = zv[0];

    if (hasMask) {
      pc->v_mul_u16(s0, s0, vm[0]);
      pc->v_div255_u16(s0);
    }

    pc->v_mov_u8_u32(d0, d0);
    pc->v_mov_u16_u32(s0, s0);
    pc->v_broadcast_f32x4(x0, pc->_getMemConst(&ct.f32_1div255));

    pc->v_cvt_i32_f32(s0, s0);
    pc->v_cvt_i32_f32(d0, d0);

    pc->v_mul_f32(s0, s0, x0);                                                                     // Sca (0..1)
    pc->v_mul_f32(d0, d0, x0);                                                                     // Dca (0..1)

    pc->vExpandAlphaPS(b0, d0);                                                                    // Da
    pc->v_mul_f32(x0, s0, b0);                                                                     // Sca.Da
    pc->v_max_f32(b0, b0, pc->simdConst(&ct.f32_1e_m3, Bcst::k32, b0));                             // max(Da, 0.001)

    pc->v_div_f32(a0, d0, b0);                                                                     // Dc <- Dca/Da
    pc->v_add_f32(d0, d0, s0);                                                                     // Dca + Sca

    pc->vExpandAlphaPS(y0, s0);                                                                    // Sa

    pc->v_sub_f32(d0, d0, x0);                                                                     // Dca + Sca.(1 - Da)
    pc->v_add_f32(s0, s0, s0);                                                                     // 2.Sca
    pc->v_mul_f32(z0, a0, pc->simdConst(&ct.f32_4, Bcst::k32, z0));                                 // 4.Dc

    pc->v_sqrt_f32(x0, a0);                                                                        // sqrt(Dc)
    pc->v_sub_f32(s0, s0, y0);                                                                     // 2.Sca - Sa

    pc->vmovaps(y0, z0);                                                                           // 4.Dc
    pc->v_mul_f32(z0, z0, a0);                                                                     // 4.Dc.Dc

    pc->v_add_f32(z0, z0, a0);                                                                     // 4.Dc.Dc + Dc
    pc->v_mul_f32(s0, s0, b0);                                                                     // (2.Sca - Sa).Da

    pc->v_sub_f32(z0, z0, y0);                                                                     // 4.Dc.Dc + Dc - 4.Dc
    pc->v_broadcast_f32x4(b0, pc->_getMemConst(&ct.f32_1));                                         // 1

    pc->v_add_f32(z0, z0, b0);                                                                     // 4.Dc.Dc + Dc - 4.Dc + 1
    pc->v_mul_f32(z0, z0, y0);                                                                     // 4.Dc(4.Dc.Dc + Dc - 4.Dc + 1)
    pc->v_cmp_f32(y0, y0, b0, x86::VCmpImm::kLE_OS);                                               // 4.Dc <= 1

    pc->v_and_f32(z0, z0, y0);
    pc->v_nand_f32(y0, y0, x0);

    pc->v_zero_f(x0);
    pc->v_or_f32(z0, z0, y0);                                                                      // (4.Dc(4.Dc.Dc + Dc - 4.Dc + 1)) or sqrt(Dc)

    pc->v_cmp_f32(x0, x0, s0, x86::VCmpImm::kLT_OS);                                               // 2.Sca - Sa > 0
    pc->v_sub_f32(z0, z0, a0);                                                                     // [[4.Dc(4.Dc.Dc + Dc - 4.Dc + 1) or sqrt(Dc)]] - Dc

    pc->v_sub_f32(b0, b0, a0);                                                                     // 1 - Dc
    pc->v_and_f32(z0, z0, x0);

    pc->v_mul_f32(b0, b0, a0);                                                                     // Dc.(1 - Dc)
    pc->v_nand_f32(x0, x0, b0);
    pc->v_and_f32(s0, s0, pc->simdConst(&ct.i_FFFFFFFF_FFFFFFFF_FFFFFFFF_0, Bcst::kNA, s0));        // Zero alpha.

    pc->v_or_f32(z0, z0, x0);
    pc->v_mul_f32(s0, s0, z0);

    pc->v_add_f32(d0, d0, s0);
    pc->v_mul_f32(d0, d0, pc->simdConst(&ct.f32_255, Bcst::k32, d0));

    pc->v_cvt_f32_i32(d0, d0);
    pc->v_packs_i32_i16(d0, d0, d0);
    pc->v_packs_i16_u8(d0, d0, d0);
    out.pc.init(d0);

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Difference
  // -------------------------------

  if (isDifference()) {
    // Dca' = Dca + Sca - 2.min(Sca.Da, Dca.Sa)
    // Da'  = Da  + Sa  -   min(Sa .Da, Da .Sa)
    if (!hasMask) {
      srcFetch(s, n, PixelFlags::kUC | PixelFlags::kUA, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& uv = s.ua;
      VecArray& dv = d.uc;

      // SPLIT.
      for (unsigned int i = 0; i < kSplit; i++) {
        VecArray sh = sv.even_odd(i);
        VecArray uh = uv.even_odd(i);
        VecArray dh = dv.even_odd(i);
        VecArray xh = xv.even_odd(i);

        pc->v_expand_alpha_16(xh, dh, kUseHi);
        pc->v_mul_u16(uh, uh, dh);
        pc->v_mul_u16(xh, xh, sh);
        pc->v_add_i16(dh, dh, sh);
        pc->v_min_u16(uh, uh, xh);
      }

      pc->v_div255_u16(uv);
      pc->v_sub_i16(dv, dv, uv);

      pc->vZeroAlphaW(uv, uv);
      pc->v_sub_i16(dv, dv, uv);
      out.uc.init(dv);
    }
    // Dca' = Dca + Sca.m - 2.min(Sca.Da, Dca.Sa).m
    // Da'  = Da  + Sa .m -   min(Sa .Da, Da .Sa).m
    else {
      srcFetch(s, n, PixelFlags::kUC, predicate);
      dstFetch(d, n, PixelFlags::kUC, predicate);

      VecArray& sv = s.uc;
      VecArray& dv = d.uc;

      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);

      // SPLIT.
      for (unsigned int i = 0; i < kSplit; i++) {
        VecArray sh = sv.even_odd(i);
        VecArray dh = dv.even_odd(i);
        VecArray xh = xv.even_odd(i);
        VecArray yh = yv.even_odd(i);

        pc->v_expand_alpha_16(yh, sh, kUseHi);
        pc->v_expand_alpha_16(xh, dh, kUseHi);
        pc->v_mul_u16(yh, yh, dh);
        pc->v_mul_u16(xh, xh, sh);
        pc->v_add_i16(dh, dh, sh);
        pc->v_min_u16(yh, yh, xh);
      }

      pc->v_div255_u16(yv);
      pc->v_sub_i16(dv, dv, yv);

      pc->vZeroAlphaW(yv, yv);
      pc->v_sub_i16(dv, dv, yv);
      out.uc.init(dv);
    }

    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Exclusion
  // ------------------------------

  if (isExclusion()) {
    // Dca' = Dca + Sca - 2.Sca.Dca
    // Da'  = Da + Sa - Sa.Da
    srcFetch(s, n, PixelFlags::kUC | (hasMask ? PixelFlags::kNone : PixelFlags::kImmutable), predicate);
    dstFetch(d, n, PixelFlags::kUC, predicate);

    VecArray& sv = s.uc;
    VecArray& dv = d.uc;

    if (hasMask) {
      pc->v_mul_u16(sv, sv, vm);
      pc->v_div255_u16(sv);
    }

    pc->v_mul_u16(xv, dv, sv);
    pc->v_add_i16(dv, dv, sv);
    pc->v_div255_u16(xv);
    pc->v_sub_i16(dv, dv, xv);

    pc->vZeroAlphaW(xv, xv);
    pc->v_sub_i16(dv, dv, xv);

    out.uc.init(dv);
    pc->x_satisfy_pixel(out, flags);
    return;
  }

  // VMaskProc - RGBA32 - Invalid
  // ----------------------------

  BL_NOT_REACHED();
}

void CompOpPart::vMaskProcRGBA32InvertMask(VecArray& vn, VecArray& vm) noexcept {
  uint32_t size = vm.size();

  if (cMaskLoopType() == CMaskLoopType::kVariant) {
    if (_mask->vn.isValid()) {
      bool ok = true;

      // TODO: [PIPEGEN] A leftover from a template-based code, I don't understand
      // it anymore and it seems it's unnecessary so verify this and all places
      // that hit `ok == false`.
      for (uint32_t i = 0; i < blMin(vn.size(), size); i++)
        if (vn[i].id() != vm[i].id())
          ok = false;

      if (ok) {
        vn.init(_mask->vn.cloneAs(vm[0]));
        return;
      }
    }
  }

  if (vn.empty())
    pc->newVecArray(vn, size, vm[0], "vn");

  pc->v_inv255_u16(vn, vm);
}

void CompOpPart::vMaskProcRGBA32InvertDone(VecArray& vn, bool mImmutable) noexcept {
  blUnused(mImmutable);

  if (cMaskLoopType() == CMaskLoopType::kVariant) {
    if (vn[0].id() == _mask->vm.id())
      pc->v_inv255_u16(vn, vn);
  }
}

} // {JIT}
} // {BLPipeline}

#endif
