//----------------------------------------------------------------------------
/** @file GoRegionBoard.cpp
    @see GoRegionBoard.h.
*/
//----------------------------------------------------------------------------

#include "SgSystem.h"
#include "GoRegionBoard.h"

#include <iostream>
#include "GoBlock.h"
#include "GoChain.h"
#include "GoEyeUtil.h"
#include "GoRegion.h"
#include "SgConnCompIterator.h"
#include "SgIO.h"
#include "SgWrite.h"

//----------------------------------------------------------------------------

const bool CHECK = SG_CHECK && true;
const bool HEAVYCHECK = SG_HEAVYCHECK && CHECK && false;
const bool DEBUG_REGION_BOARD = false;

const int   kRemoveRegion = 2000,
            kAddRegion = 2001,
            kRemoveBlock = 2002,
            kAddBlock = 2003,
            kAddStoneToRegion = 2004,
            kAddStoneToBlock = 2005;
//----------------------------------------------------------------------------

bool GoRegionBoard::IsSafeBlock(SgPoint p) const
{
    return BlockAt(p)->IsSafe();
}

void GoRegionBoard::SetToSafe(SgPoint p) const
{
    BlockAt(p)->SetToSafe();
}

void GoRegionBoard::Fini()
{
    SG_ASSERT(s_alloc == s_free);
}


void GoRegionBoard::SetSafeFlags(const SgBWSet& safe)
{
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            if ((*it)->Points().Overlaps(safe[color]))
                (*it)->SetToSafe();
        for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
            if ((*it)->Stones().Overlaps(safe[color]))
                (*it)->SetToSafe();
    }
}

//----------------------------------------------------------------------------

GoRegionBoard::GoRegionBoard(const GoBoard& board)
    : m_board(board),
      m_region(SgPointArray<GoRegion*>(0)),
      m_block(0),
      m_invalid(true),
      m_computedHealthy(false),
      m_boardSize(board.Size())
{
    m_code.Invalidate();
    m_chainsCode.Invalidate();
    GenBlocksRegions();
    ++s_alloc;
}

GoRegionBoard::~GoRegionBoard()
{
    Clear();
    ++s_free;
}

void GoRegionBoard::Clear()
{
    if (DEBUG_REGION_BOARD)
        SgDebug() << "Clear\n";
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoBlock> it1(AllBlocks(color)); it1; ++it1)
            delete *it1;
        for (SgListIteratorOf<GoRegion> it2(AllRegions(color)); it2; ++it2)
            delete *it2;
        for (SgListIteratorOf<GoChain> it3(AllChains(color)); it3; ++it3)
            delete *it3;
    }
    m_allBlocks[SG_BLACK].Clear();
    m_allBlocks[SG_WHITE].Clear();
    m_allRegions[SG_BLACK].Clear();
    m_allRegions[SG_WHITE].Clear();
    m_allChains[SG_BLACK].Clear();
    m_allChains[SG_WHITE].Clear();
    m_stack.Clear();
    m_code.Invalidate();
    m_invalid = true;
    m_computedHealthy = false;
    m_boardSize = m_board.Size();

    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgPoint p = SgPointUtil::Pt(1, 1);
             p <= SgPointUtil::Pt(SG_MAX_SIZE, SG_MAX_SIZE);
             ++p)
        {
            m_region[color][p] = 0;
        }
    }
    for (SgPoint p = SgPointUtil::Pt(1, 1);
         p <= SgPointUtil::Pt(SG_MAX_SIZE, SG_MAX_SIZE); ++p)
        m_block[p] = 0;
}

void GoRegionBoard::UpdateBlock(int move, SgBlackWhite moveColor)
{
    SgPoint anchor = Board().Anchor(move); // board is already up to date.
    bool done = false;
    const int size = Board().Size();
    if (! Board().IsSingleStone(move)) // find old neighbor blocks
    {
        SgListOf<GoBlock> old;
        for (SgNb4Iterator it(move); it; ++it)
        {
            if (IsColor(*it, moveColor))
                old.Include(BlockAt(*it));
        }
        if (old.IsLength(1)) // append to single neighbor block
        {
            GoBlock* b = old.Top();
            AppendStone(b, move);
            done = true;
        }
        else // delete old, recompute
        {
            for (SgListIteratorOf<GoBlock> it(old); it; ++it)
                RemoveBlock(*it, true, true);
        }
    }

    if (! done) // create new block.
    {
        GoBlock* b = GenBlock(anchor, moveColor);
        SgPointSet area(b->Stones().Border(size));
        // link it to neighbor regions
        SgListOf<GoRegion> regions;
        RegionsAt(area, moveColor, &regions);
        for (SgListIteratorOf<GoRegion> it(regions); it; ++it)
            (*it)->BlocksNonConst().Append(b);
    }
}

void GoRegionBoard::AppendStone(GoBlock* b, SgPoint move)
{
    m_block[move] = b;
    b->AddStone(move);
    m_stack.PushInt(move);
    m_stack.PushPtrEvent(kAddStoneToBlock, b);
}


void GoRegionBoard::OnExecutedMove(GoPlayerMove move)
{
    OnExecutedUncodedMove(move.Point(), move.Color());
}

void GoRegionBoard::ExecuteMovePrologue()
{
    if (! UpToDate()) // full recomputation
    {
        if (DEBUG_REGION_BOARD)
            SgDebug() << "recompute everything\n";
        GenBlocksRegions();
    }
}

void GoRegionBoard::OnExecutedUncodedMove(int move, SgBlackWhite moveColor)
{
    if (DEBUG_REGION_BOARD)
        SgDebug() << "OnExecutedUncodedMove " << SgWritePoint(move) << '\n';
    {
        m_stack.StartMoveInfo();
        if (move != SG_PASS)
        {
            SG_ASSERT(! Board().LastMoveInfo(isSuicide));
            // can't handle yet,
            // should be forbidden anyway. AR: allowed in Chinese rules.
            bool fWasCapture = Board().LastMoveInfo(isCapturing);

            UpdateBlock(move, moveColor);

            {
                GoRegion* r = PreviousRegionAt(move, moveColor);
                bool split = GoEyeUtil::IsSplitPt(move, r->Points());

                r->OnAddStone(move);
                PushStone(r, move);
                SgPointSet points = r->Points();
                // needed even after RemoveRegion(r).
                if (split || points.IsEmpty())
                // must remove old region before generating new ones,
                // because removing clears m_anchor[]
                    RemoveRegion(r);

                if (split) // find new regions
                {
                    for (SgConnCompIterator it(points, Board().Size());
                         it; ++it)
                        GenRegion(*it, moveColor);
                }
            }

            if (fWasCapture)
            {
            //  FindNewNeighborRegions(move, moveColor);
                MergeAdjacentAndAddBlock(move, OppBW(moveColor));
            }

            m_code = Board().GetHashCode();
            if (HEAVYCHECK)
                CheckConsistency();
        }
    }

    {
        for (SgBWIterator it; it; ++it)
        {
            SgBlackWhite color(*it);
            for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            {   GoRegion* r1 = *it;
                if (! r1->IsValid())
                    r1->ComputeBasicFlags();
            }
        }
    }
}

void GoRegionBoard::CheckConsistency() const
{
    SG_ASSERT(CHECK && UpToDate());
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        SgPointSet blockArea, regionArea;
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
        {
            SG_ASSERT((*it)->Points().Disjoint(regionArea));
            SG_ASSERT((*it)->Points().Disjoint(blockArea));
            (*it)->CheckConsistency();
            regionArea |= (*it)->Points();
        }
        for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
        {
            SG_ASSERT((*it)->Stones().Disjoint(regionArea));
            SG_ASSERT((*it)->Stones().Disjoint(blockArea));
            for (SgSetIterator it2((*it)->Stones()); it2; ++it2)
                SG_ASSERT(m_block[*it2] == *it);
            (*it)->CheckConsistency();
            blockArea |= (*it)->Stones();
        }
        SG_ASSERT(blockArea.Disjoint(regionArea));
        SG_ASSERT((blockArea | regionArea) == Board().AllPoints());

        // check m_region array
        for (SgPoint p = SgPointUtil::Pt(1, 1);
             p <= SgPointUtil::Pt(SG_MAX_SIZE, SG_MAX_SIZE);
             ++p)
        {
            SG_ASSERT((m_region[color][p] == 0) != regionArea.Contains(p));
        }

    }
    // check block array
    for (SgPoint p = SgPointUtil::Pt(1, 1);
         p <= SgPointUtil::Pt(SG_MAX_SIZE, SG_MAX_SIZE); ++p)
    {
        GoBlock* b = m_block[p];
        SG_UNUSED(b);
        SG_ASSERT((b != 0) == Board().Occupied().Contains(p));
    }
}

void GoRegionBoard::PushRegion(int type, GoRegion* r)
{
    m_stack.PushPtrEvent(type, r);
}

void GoRegionBoard::PushBlock(int type, GoBlock* b)
{
    m_stack.PushPtrEvent(type, b);
}

void GoRegionBoard::PushStone(GoRegion* r, SgPoint move)
{
    m_stack.PushInt(move);
    m_stack.PushPtrEvent(kAddStoneToRegion, r);
    m_region[r->Color()][move] = 0;
}

void GoRegionBoard::RemoveRegion(GoRegion* r, bool isExecute)
{
    SgBlackWhite color = r->Color();

    for (SgSetIterator reg(r->Points()); reg; ++reg)
    {
        m_region[color][*reg] = 0; // remove link of region.
    }

    bool found = m_allRegions[r->Color()].Exclude(r);
    SG_UNUSED(found);
    SG_ASSERT(found);

    // remove from blocks.
    for (SgListIteratorOf<GoBlock> it(r->Blocks()); it; ++it)
        (*it)->RemoveRegion(r);

    if (isExecute)
        PushRegion(kRemoveRegion, r);
    else // undo
        delete r;
}

void GoRegionBoard::SetRegionArrays(GoRegion* r)
{
    SgBlackWhite color = r->Color();

    for (SgSetIterator reg(r->Points()); reg; ++reg)
    {
        m_region[color][*reg] = r;
    }
}

void GoRegionBoard::RemoveBlock(GoBlock* b, bool isExecute,
                                     bool removeFromRegions)
{
    SgBlackWhite color = b->Color();
    for (SgSetIterator it(b->Stones()); it; ++it)
        m_block[*it] = 0;

    bool found = m_allBlocks[color].Exclude(b);
    SG_UNUSED(found);
    SG_ASSERT(found);
    const int size = Board().Size();
    // remove from regions.
    SgPointSet area(b->Stones().Border(size));
    SgListOf<GoRegion> regions;
    if (removeFromRegions)
    {
        RegionsAt(area, color, &regions);
        for (SgListIteratorOf<GoRegion> it(regions); it; ++it)
        {
            (*it)->RemoveBlock(b);
            if (isExecute)
                m_stack.PushPtr(*it);
        }
    }
    if (isExecute)
    {
        m_stack.PushInt(regions.Length()); // 0 if ! removeFromRegions
        PushBlock(kRemoveBlock, b);
    }
    else
        delete b;
}

void GoRegionBoard::AddBlock(GoBlock* b, bool isExecute)
{
    SgBlackWhite color = b->Color();
    AllBlocks(color).Append(b);
    for (GoBoard::StoneIterator it(Board(), b->Anchor()); it; ++it)
        m_block[*it] = b;
    if (isExecute)
        PushBlock(kAddBlock, b);
}

GoBlock* GoRegionBoard::GenBlock(SgPoint anchor, SgBlackWhite color)
{
    GoBlock* b = new GoBlock(color, anchor, Board());
    AddBlock(b);
    return b;
}

void
GoRegionBoard::PreviousBlocksAt(const SgList<SgPoint>& area,
                                SgBlackWhite color,
                                SgListOf<GoBlock>* captures) const
{
    SG_UNUSED(color);

    for (SgListIterator<SgPoint> it(area); it; ++it)
    {
        GoBlock* b = m_block[*it];
        if (b)
            captures->Include(b);
    }
}

#if UNUSED
void GoRegionBoard::BlockToRegion(GoBlock* b)
// convert a captured block into an opponent region.
{
    GoRegion* r = new GoRegion(Board(), b->Stones(), OppBW(b->Color()));
    SetRegionArrays(r);
    // AR anchor array should still be OK, don't need to set it.
}

void GoRegionBoard::FindNewNeighborRegions(SgPoint move,
                                                BlackWhite moveColor)
{ // move was capture -> new region for each captured block.

    SgList<SgPoint> nb;
    for (Nb4Iterator it(move); it; ++it)
        if (Board().IsEmpty(*it))
            nb.Append(*it);

    SgListOf<GoBlock> captures;
    PreviousBlocksAt(nb, OppBW(moveColor), &captures);
    ASSERT(captures.NonEmpty());

    for (SgListIteratorOf<GoBlock> it(captures); it; ++it)
        BlockToRegion(*it);
}
#endif


void GoRegionBoard::RegionsAt(const SgPointSet& area, SgBlackWhite color,
                                   SgListOf<GoRegion>* regions) const
{
    for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
        if ((*it)->Points().Overlaps(area))
            regions->Append(*it);
}

void GoRegionBoard::AdjacentRegions(const SgList<SgPoint>& anchors,
                                         SgBlackWhite color,
                                         SgListOf<GoRegion>* regions) const
{
    for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
        if ((*it)->AdjacentToSomeBlock(anchors))
            regions->Append(*it);
}

GoRegion* GoRegionBoard::MergeAll(const SgListOf<GoRegion>& regions,
                                       const SgPointSet& captured,
                                       SgBlackWhite color)
{
    SgPointSet area(captured);
    for (SgListIteratorOf<GoRegion> it(regions); it; ++it)
        area |= (*it)->Points();
    {for (SgListIteratorOf<GoRegion> it(regions); it; ++it)
        RemoveRegion(*it);
    }
    GoRegion* r = GenRegion(area, color);

    return r;
}

void GoRegionBoard::MergeAdjacentAndAddBlock(SgPoint move,
                                                  SgBlackWhite capturedColor)
{
    SgList<SgPoint> nb;
    for (SgNb4Iterator it(move); it; ++it)
        if (Board().IsEmpty(*it))
            nb.Append(*it);

    SgListOf<GoBlock> captures;
    PreviousBlocksAt(nb, capturedColor, &captures);
    SG_ASSERT(captures.NonEmpty());

    SgPointSet captured;
    {for (SgListIteratorOf<GoBlock> it(captures); it; ++it)
        captured |= (*it)->Stones();
    }
    SgListOf<GoRegion> adj;
    const int size = Board().Size();
    RegionsAt(captured.Border(size), capturedColor, &adj);
    SG_ASSERT(adj.NonEmpty());
    GoRegion* r = MergeAll(adj, captured, capturedColor);
    SG_UNUSED(r);

    for (SgListIteratorOf<GoBlock> it(captures); it; ++it)
        RemoveBlock(*it, true, false);
        // don't remove from regions; already gone.
}

void GoRegionBoard::OnUndoneMove()
// Called after a move has been undone. The board is guaranteed to be in
// a legal state.
{
    //ASSERT(false); // incremental code is incomplete, do not call
    if (DEBUG_REGION_BOARD)
        SgDebug() << "OnUndoneMove " << '\n';

    const bool kUndo = false;
    SgListOf<GoRegion> changed;

    for (int val = m_stack.PopEvent(); val != kNextMove;
         val = m_stack.PopEvent())
    {

        switch (val)
        {
            case kRemoveRegion:
            {   GoRegion* r = static_cast<GoRegion*>(m_stack.PopPtr());
                AddRegion(r, kUndo);
                changed.Insert(r);
            }
            break;
            case kAddRegion:
            {   GoRegion* r = static_cast<GoRegion*>(m_stack.PopPtr());
                RemoveRegion(r, kUndo);
            }
            break;
            case kRemoveBlock:
            {   GoBlock* b = static_cast<GoBlock*>(m_stack.PopPtr());
                AddBlock(b, kUndo);
                for (int nu = m_stack.PopInt(); nu > 0; --nu)
                {
                    GoRegion* r = static_cast<GoRegion*>(m_stack.PopPtr());
                    if (CHECK)
                        SG_ASSERT(! r->Blocks().Contains(b));
                    r->BlocksNonConst().Append(b);
                    changed.Insert(r);
                }
            }
            break;
            case kAddBlock:
            {   GoBlock* b = static_cast<GoBlock*>(m_stack.PopPtr());
                RemoveBlock(b, kUndo, true);
            }
            break;
            case kAddStoneToRegion:
            {   GoRegion* r = static_cast<GoRegion*>(m_stack.PopPtr());
                SgPoint p = m_stack.PopInt();
                r->OnRemoveStone(p);
                m_region[r->Color()][p] = r;
                changed.Insert(r);
            }
            break;
            case kAddStoneToBlock:
            {   GoBlock* b = static_cast<GoBlock*>(m_stack.PopPtr());
                SgPoint p = m_stack.PopInt();
                b->RemoveStone(p);
                m_block[p] = 0;
            }
            break;
            default:
                SG_ASSERT(false);
        }
    }

    for (SgListIteratorOf<GoRegion> it(changed); it; ++it)
    {
        (*it)->ResetNonBlockFlags();
        (*it)->ComputeBasicFlags();
    }

    if (HEAVYCHECK)
    {
        for (SgBWIterator it; it; ++it)
        {
            SgBlackWhite color(*it);
            for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            {
                const GoRegion* r = *it;
                SG_UNUSED(r);
                SG_ASSERT(r->IsValid());
            }
        }
    }

    m_code = Board().GetHashCode();
    if (HEAVYCHECK)
        CheckConsistency();
}

void GoRegionBoard::ReInitializeBlocksRegions()
{
    SG_ASSERT(UpToDate());

    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
            (*it)->ReInitialize();
        for (SgListIteratorOf<GoRegion> it2(AllRegions(color)); it2; ++it2)
            (*it2)->ReInitialize();
    }
}

GoRegion* GoRegionBoard::GenRegion(const SgPointSet& area,
                                        SgBlackWhite color)
{
    GoRegion* r = new GoRegion(Board(), area, color);
    AddRegion(r);
    return r;
}

void GoRegionBoard::AddRegion(GoRegion* r, bool isExecute)
{
    SetRegionArrays(r);

    if (isExecute) // not needed if restoring during undo.
    {
        r->FindBlocks(*this);
        r->ComputeBasicFlags();
    }

    SgBlackWhite color = r->Color();
    if (HEAVYCHECK)
        SG_ASSERT(! AllRegions(color).Contains(r));
    AllRegions(color).Append(r);

    if (isExecute)
        PushRegion(kAddRegion, r);
}

void GoRegionBoard::FindBlocksWithEye()
{
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            if ((*it)->Blocks().IsLength(1))
            {
                GoBlock* b = (*it)->Blocks().Top();
                (b)->TestFor1Eye(*it);
            }
    }
}

void GoRegionBoard::GenBlocksRegions()
{
    if (UpToDate())
        return;

    Clear();
    GenBlocks();

    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgConnCompIterator it(AllPoints() - All(color), Board().Size());
             it; ++it)
            GenRegion(*it, color);
    }

    FindBlocksWithEye();

    m_code = Board().GetHashCode();
    m_invalid = false;
    if (HEAVYCHECK)
        CheckConsistency();
}

void GoRegionBoard::GenChains()
{
    if (ChainsUpToDate())
        return;

    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        SG_ASSERT(AllChains(color).IsEmpty());

        for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
            AllChains(color).Append(new GoChain(*it, Board()));
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            (*it)->FindChains(*this);
    }
    m_chainsCode = Board().GetHashCode();
}

void GoRegionBoard::WriteBlocks(std::ostream& stream) const
{
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
            stream << **it;
    }
}

void GoRegionBoard::WriteRegions(std::ostream& stream) const
{
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            (*it)->Write(stream);
    }
}

GoBlock* GoRegionBoard::GetBlock(const SgPointSet& boundary,
                                 SgBlackWhite color) const
{
    SG_ASSERT(UpToDate());

    for (SgListIteratorOf<GoBlock> it(AllBlocks(color)); it; ++it)
    {   if (boundary.SubsetOf((*it)->Stones()))
        /* */ return *it; /* */
    }

    SgDebug() << "ERROR: no block on set";
    const int size = Board().Size();
    boundary.Write(SgDebug(), size);
    SgDebug() << "blocks:";
    for (SgListIteratorOf<GoBlock> it(AllBlocks(color));it; ++it)
        (*it)->Stones().Write(SgDebug(), size);

    SG_ASSERT(false);
    return 0;
}

GoChain* GoRegionBoard::ChainAt(SgPoint p) const
{
    SG_ASSERT(ChainsUpToDate());
    const SgBlackWhite color = Board().GetStone(p);

    for (SgListIteratorOf<GoChain> it(AllChains(color));it; ++it)
        if ((*it)->Stones().Contains(p))
            /* */ return *it; /* */

    SG_ASSERT(false);
    return 0;
}

void GoRegionBoard::GenBlocks()
{
    for (GoBlockIterator it(Board()); it; ++it)
        GenBlock(*it, Board().GetStone(*it));
}

void GoRegionBoard::SetComputedFlagForAll(GoRegionFlag flag)
{
    // mark all regions that their safety has been computed.
    for (SgBWIterator it; it; ++it)
    {
        SgBlackWhite color(*it);
        for (SgListIteratorOf<GoRegion> it(AllRegions(color)); it; ++it)
            (*it)->SetComputedFlag(flag);
    }
}

void GoRegionBoard::SetComputedHealthy()
{
    m_computedHealthy = true;
}

//----------------------------------------------------------------------------

int GoRegionBoard::s_alloc = 0;
int GoRegionBoard::s_free = 0;
