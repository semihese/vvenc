/* -----------------------------------------------------------------------------
Software Copyright License for the Fraunhofer Software Library VVenc

(c) Copyright (2019-2020) Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V. 

1.    INTRODUCTION

The Fraunhofer Software Library VVenc (“Fraunhofer Versatile Video Encoding Library”) is software that implements (parts of) the Versatile Video Coding Standard - ITU-T H.266 | MPEG-I - Part 3 (ISO/IEC 23090-3) and related technology. 
The standard contains Fraunhofer patents as well as third-party patents. Patent licenses from third party standard patent right holders may be required for using the Fraunhofer Versatile Video Encoding Library. It is in your responsibility to obtain those if necessary. 

The Fraunhofer Versatile Video Encoding Library which mean any source code provided by Fraunhofer are made available under this software copyright license. 
It is based on the official ITU/ISO/IEC VVC Test Model (VTM) reference software whose copyright holders are indicated in the copyright notices of its source files. The VVC Test Model (VTM) reference software is licensed under the 3-Clause BSD License and therefore not subject of this software copyright license.

2.    COPYRIGHT LICENSE

Internal use of the Fraunhofer Versatile Video Encoding Library, in source and binary forms, with or without modification, is permitted without payment of copyright license fees for non-commercial purposes of evaluation, testing and academic research. 

No right or license, express or implied, is granted to any part of the Fraunhofer Versatile Video Encoding Library except and solely to the extent as expressly set forth herein. Any commercial use or exploitation of the Fraunhofer Versatile Video Encoding Library and/or any modifications thereto under this license are prohibited.

For any other use of the Fraunhofer Versatile Video Encoding Library than permitted by this software copyright license You need another license from Fraunhofer. In such case please contact Fraunhofer under the CONTACT INFORMATION below.

3.    LIMITED PATENT LICENSE

As mentioned under 1. Fraunhofer patents are implemented by the Fraunhofer Versatile Video Encoding Library. If You use the Fraunhofer Versatile Video Encoding Library in Germany, the use of those Fraunhofer patents for purposes of testing, evaluating and research and development is permitted within the statutory limitations of German patent law. However, if You use the Fraunhofer Versatile Video Encoding Library in a country where the use for research and development purposes is not permitted without a license, you must obtain an appropriate license from Fraunhofer. It is Your responsibility to check the legal requirements for any use of applicable patents.    

Fraunhofer provides no warranty of patent non-infringement with respect to the Fraunhofer Versatile Video Encoding Library.


4.    DISCLAIMER

The Fraunhofer Versatile Video Encoding Library is provided by Fraunhofer "AS IS" and WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, including but not limited to the implied warranties fitness for a particular purpose. IN NO EVENT SHALL FRAUNHOFER BE LIABLE for any direct, indirect, incidental, special, exemplary, or consequential damages, including but not limited to procurement of substitute goods or services; loss of use, data, or profits, or business interruption, however caused and on any theory of liability, whether in contract, strict liability, or tort (including negligence), arising in any way out of the use of the Fraunhofer Versatile Video Encoding Library, even if advised of the possibility of such damage.

5.    CONTACT INFORMATION

Fraunhofer Heinrich Hertz Institute
Attention: Video Coding & Analytics Department
Einsteinufer 37
10587 Berlin, Germany
www.hhi.fraunhofer.de/vvc
vvc@hhi.fraunhofer.de
----------------------------------------------------------------------------- */


/** \file     EncSearch.cpp
 *  \brief    encoder intra search class
 */

#include "IntraSearch.h"
#include "EncPicture.h"
#include "CommonLib/CommonDef.h"
#include "CommonLib/Rom.h"
#include "CommonLib/Picture.h"
#include "CommonLib/UnitTools.h"
#include "CommonLib/dtrace_next.h"
#include "CommonLib/dtrace_buffer.h"
#include "CommonLib/Reshape.h"
#include <math.h>
#include "../../../include/vvenc/EncCfg.h"

//! \ingroup EncoderLib
//! \{

namespace vvenc {

#define PLTCtx(c) SubCtx( Ctx::Palette, c )
IntraSearch::IntraSearch()
  : m_pTempCS       (nullptr)
  , m_pBestCS       (nullptr)
  , m_pSaveCS       (nullptr)
  , m_pcEncCfg      (nullptr)
  , m_pcTrQuant     (nullptr)
  , m_pcRdCost      (nullptr)
  , m_CABACEstimator(nullptr)
  , m_CtxCache      (nullptr)
{
}

void IntraSearch::init(const EncCfg &encCfg, TrQuant *pTrQuant, RdCost *pRdCost, SortedPelUnitBufs<SORTED_BUFS> *pSortedPelUnitBufs, XUCache &unitCache )
{
  IntraPrediction::init( encCfg.m_internChromaFormat, encCfg.m_internalBitDepth[ CH_L ] );

  m_pcEncCfg          = &encCfg;
  m_pcTrQuant         = pTrQuant;
  m_pcRdCost          = pRdCost;
  m_SortedPelUnitBufs = pSortedPelUnitBufs;

  const ChromaFormat chrFormat = encCfg.m_internChromaFormat;
  const int maxCUSize          = encCfg.m_CTUSize;

  const int numWidths  = MAX_CU_SIZE_IDX;
  const int numHeights = MAX_CU_SIZE_IDX;

  m_pBestCS = new CodingStructure**[numWidths];
  m_pTempCS = new CodingStructure**[numWidths];

  for( int wIdx = 0; wIdx < numWidths; wIdx++ )
  {
    m_pBestCS[wIdx] = new CodingStructure*[numHeights];
    m_pTempCS[wIdx] = new CodingStructure*[numHeights];

    for( int hIdx = 0; hIdx < numHeights; hIdx++ )
    {
      if( wIdx < 2 || hIdx < 2 )
      {
        m_pBestCS[wIdx][hIdx] = nullptr;
        m_pTempCS[wIdx][hIdx] = nullptr;
        continue;
      }

      m_pBestCS[wIdx][hIdx] = new CodingStructure( unitCache, nullptr );
      m_pTempCS[wIdx][hIdx] = new CodingStructure( unitCache, nullptr );

      Area area = Area( 0, 0, 1<<wIdx, 1<<hIdx );
      m_pBestCS[wIdx][hIdx]->create( chrFormat, area, false );
      m_pTempCS[wIdx][hIdx]->create( chrFormat, area, false );
    }
  }

  const int uiNumSaveLayersToAllocate = 3;
  m_pSaveCS = new CodingStructure*[uiNumSaveLayersToAllocate];
  for( int layer = 0; layer < uiNumSaveLayersToAllocate; layer++ )
  {
    m_pSaveCS[ layer ] = new CodingStructure( unitCache, nullptr );
    m_pSaveCS[ layer ]->create( chrFormat, Area( 0, 0, maxCUSize, maxCUSize ), false );
    m_pSaveCS[ layer ]->initStructData();
  }

}


void IntraSearch::destroy()
{
  if ( m_pSaveCS )
  {
    const int uiNumSaveLayersToAllocate = 3;
    for( int layer = 0; layer < uiNumSaveLayersToAllocate; layer++ )
    {
      if ( m_pSaveCS[ layer ] ) { m_pSaveCS[ layer ]->destroy(); delete m_pSaveCS[ layer ]; }
    }
    delete[] m_pSaveCS;
    m_pSaveCS = nullptr;
  }

  const int numWidths  = MAX_CU_SIZE_IDX;
  const int numHeights = MAX_CU_SIZE_IDX;
  for( int wIdx = 0; wIdx < numWidths; wIdx++ )
  {
    for( int hIdx = 0; hIdx < numHeights; hIdx++ )
    {
      if ( m_pBestCS && m_pBestCS[ wIdx ] && m_pBestCS[ wIdx ][ hIdx ] ) { m_pBestCS[ wIdx ][ hIdx ]->destroy(); delete m_pBestCS[ wIdx ][ hIdx ]; }
      if ( m_pTempCS && m_pTempCS[ wIdx ] && m_pTempCS[ wIdx ][ hIdx ] ) { m_pTempCS[ wIdx ][ hIdx ]->destroy(); delete m_pTempCS[ wIdx ][ hIdx ]; }
    }
    if ( m_pBestCS  && m_pBestCS[ wIdx ]  ) { delete[] m_pBestCS[ wIdx ];  }
    if ( m_pTempCS  && m_pTempCS[ wIdx ]  ) { delete[] m_pTempCS[ wIdx ];  }
  }
  if ( m_pBestCS  ) { delete[] m_pBestCS;  m_pBestCS  = nullptr; }
  if ( m_pTempCS  ) { delete[] m_pTempCS;  m_pTempCS  = nullptr; }

}

IntraSearch::~IntraSearch()
{
  destroy();
}

void IntraSearch::setCtuEncRsrc( CABACWriter* cabacEstimator, CtxCache *ctxCache )
{
  m_CABACEstimator = cabacEstimator;
  m_CtxCache       = ctxCache;
}

//////////////////////////////////////////////////////////////////////////
// INTRA PREDICTION
//////////////////////////////////////////////////////////////////////////
static constexpr double COST_UNKNOWN = -65536.0;

double IntraSearch::xFindInterCUCost( CodingUnit &cu )
{
  if( cu.isConsIntra() && !cu.slice->isIntra() )
  {
    //search corresponding inter CU cost
    for( int i = 0; i < m_numCuInSCIPU; i++ )
    {
      if( cu.lumaPos() == m_cuAreaInSCIPU[i].pos() && cu.lumaSize() == m_cuAreaInSCIPU[i].size() )
      {
        return m_cuCostInSCIPU[i];
      }
    }
  }
  return COST_UNKNOWN;
}

void IntraSearch::xEstimateLumaRdModeList(int& numModesForFullRD,
  static_vector<ModeInfo, FAST_UDI_MAX_RDMODE_NUM>& RdModeList,
  static_vector<ModeInfo, FAST_UDI_MAX_RDMODE_NUM>& HadModeList,
  static_vector<double, FAST_UDI_MAX_RDMODE_NUM>& CandCostList,
  static_vector<double, FAST_UDI_MAX_RDMODE_NUM>& CandHadList, CodingUnit& cu, bool testMip )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 0, g_timeProfiler, P_INTRA_EST_RD_CAND_LUMA, cu.cs, CH_L );
  const uint16_t intra_ctx_size = Ctx::IntraLumaMpmFlag.size() + Ctx::IntraLumaPlanarFlag.size() + Ctx::MultiRefLineIdx.size() + Ctx::ISPMode.size() + Ctx::MipFlag.size();
  const TempCtx  ctxStartIntraCtx(m_CtxCache, SubCtx(CtxSet(Ctx::IntraLumaMpmFlag(), intra_ctx_size), m_CABACEstimator->getCtx()));
  const double   sqrtLambdaForFirstPass = m_pcRdCost->getMotionLambda() * FRAC_BITS_SCALE;
  const int numModesAvailable = NUM_LUMA_MODE; // total number of Intra modes

  CHECK(numModesForFullRD >= numModesAvailable, "Too many modes for full RD search");

  PredictionUnit &pu = *cu.pu;
  const SPS& sps     = *cu.cs->sps;
  const bool fastMip = sps.MIP && m_pcEncCfg->m_useFastMIP;

  // this should always be true
  CHECK( !pu.Y().valid(), "PU is not valid" );

  const CompArea& area = pu.Y();

  const UnitArea localUnitArea(area.chromaFormat, Area(0, 0, area.width, area.height));
  if( testMip)
  {
    numModesForFullRD += fastMip? std::max(numModesForFullRD, floorLog2(std::min(pu.lwidth(), pu.lheight())) - m_pcEncCfg->m_useFastMIP) : numModesForFullRD;
    m_SortedPelUnitBufs->prepare( localUnitArea, numModesForFullRD + 1 );
  }
  else
  {
    m_SortedPelUnitBufs->prepare( localUnitArea, numModesForFullRD );
  }

  CPelBuf piOrg   = cu.cs->getOrgBuf(COMP_Y);
  PelBuf piPred  = m_SortedPelUnitBufs->getTestBuf(COMP_Y);

  const ReshapeData& reshapeData = cu.cs->picture->reshapeData;
  if (cu.cs->picHeader->lmcsEnabled && reshapeData.getCTUFlag())
  {
    piOrg = cu.cs->getRspOrgBuf();
  }
  DistParam distParam    = m_pcRdCost->setDistParam( piOrg, piPred, sps.bitDepths[ CH_L ], DF_HAD_2SAD); // Use HAD (SATD) cost

  const int numHadCand = (testMip ? 2 : 1) * 3;

  //*** Derive (regular) candidates using Hadamard
  cu.mipFlag = false;
  pu.multiRefIdx = 0;

  //===== init pattern for luma prediction =====
  initIntraPatternChType(cu, pu.Y(), true);

  bool satdChecked[NUM_INTRA_MODE] = { false };

  for( unsigned mode = 0; mode < numModesAvailable; mode++ )
  {
    // Skip checking extended Angular modes in the first round of SATD
    if( mode > DC_IDX && ( mode & 1 ) )
    {
      continue;
    }

    pu.intraDir[0] = mode;

    initPredIntraParams(pu, pu.Y(), sps);
    distParam.cur.buf = piPred.buf = m_SortedPelUnitBufs->getTestBuf().Y().buf;
    predIntraAng( COMP_Y, piPred, pu);

    // Use the min between SAD and HAD as the cost criterion
    // SAD is scaled by 2 to align with the scaling of HAD
    Distortion minSadHad = distParam.distFunc(distParam);

    uint64_t fracModeBits = xFracModeBitsIntraLuma( pu );

    //restore ctx
    m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::IntraLumaMpmFlag(), intra_ctx_size), ctxStartIntraCtx);

    double cost = ( double ) minSadHad + (double)fracModeBits * sqrtLambdaForFirstPass;
    DTRACE(g_trace_ctx, D_INTRA_COST, "IntraHAD: %u, %llu, %f (%d)\n", minSadHad, fracModeBits, cost, mode);

    int insertPos = -1;
    updateCandList( ModeInfo(false, false, 0, NOT_INTRA_SUBPARTITIONS, mode), cost, RdModeList, CandCostList, numModesForFullRD, &insertPos );
    updateCandList( ModeInfo(false, false, 0, NOT_INTRA_SUBPARTITIONS, mode), (double)minSadHad, HadModeList, CandHadList,  numHadCand );
    m_SortedPelUnitBufs->insert( insertPos, (int)RdModeList.size() );

    satdChecked[mode] = true;
  }

  std::vector<ModeInfo> parentCandList( RdModeList.cbegin(), RdModeList.cend());

  // Second round of SATD for extended Angular modes
  for (unsigned modeIdx = 0; modeIdx < numModesForFullRD; modeIdx++)
  {
    unsigned parentMode = parentCandList[modeIdx].modeId;
    if (parentMode > (DC_IDX + 1) && parentMode < (NUM_LUMA_MODE - 1))
    {
      for (int subModeIdx = -1; subModeIdx <= 1; subModeIdx += 2)
      {
        unsigned mode = parentMode + subModeIdx;

        if( ! satdChecked[mode])
        {
          pu.intraDir[0] = mode;

          initPredIntraParams(pu, pu.Y(), sps);
          distParam.cur.buf = piPred.buf = m_SortedPelUnitBufs->getTestBuf().Y().buf;
          predIntraAng(COMP_Y, piPred, pu );

          // Use the min between SAD and SATD as the cost criterion
          // SAD is scaled by 2 to align with the scaling of HAD
          Distortion minSadHad = distParam.distFunc(distParam);

          uint64_t fracModeBits = xFracModeBitsIntraLuma( pu );
          //restore ctx
          m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::IntraLumaMpmFlag(), intra_ctx_size), ctxStartIntraCtx);

          double cost = (double) minSadHad + (double) fracModeBits * sqrtLambdaForFirstPass;
//          DTRACE(g_trace_ctx, D_INTRA_COST, "IntraHAD2: %u, %llu, %f (%d)\n", minSadHad, fracModeBits, cost, mode);

          int insertPos = -1;
          updateCandList( ModeInfo( false, false, 0, NOT_INTRA_SUBPARTITIONS, mode ), cost, RdModeList, CandCostList, numModesForFullRD, &insertPos );
          updateCandList( ModeInfo( false, false, 0, NOT_INTRA_SUBPARTITIONS, mode ), (double)minSadHad, HadModeList, CandHadList,  numHadCand );
          m_SortedPelUnitBufs->insert(insertPos, (int)RdModeList.size());

          satdChecked[mode] = true;
        }
      }
    }
  }

  const bool isFirstLineOfCtu = (((pu.block(COMP_Y).y)&((pu.cs->sps)->CTUSize - 1)) == 0);
  if( m_pcEncCfg->m_MRL && ! isFirstLineOfCtu )
  {
    pu.multiRefIdx = 1;
    unsigned  multiRefMPM [NUM_MOST_PROBABLE_MODES];
    PU::getIntraMPMs(pu, multiRefMPM);

    for (int mRefNum = 1; mRefNum < MRL_NUM_REF_LINES; mRefNum++)
    {
      int multiRefIdx = MULTI_REF_LINE_IDX[mRefNum];

      pu.multiRefIdx = multiRefIdx;
      initIntraPatternChType(cu, pu.Y(), true);

      for (int x = 1; x < NUM_MOST_PROBABLE_MODES; x++)
      {
        pu.intraDir[0] = multiRefMPM[x];
        initPredIntraParams(pu, pu.Y(), sps);
        distParam.cur.buf = piPred.buf = m_SortedPelUnitBufs->getTestBuf().Y().buf;
        predIntraAng(COMP_Y, piPred, pu);

        // Use the min between SAD and SATD as the cost criterion
        // SAD is scaled by 2 to align with the scaling of HAD
        Distortion minSadHad = distParam.distFunc(distParam);

        // NB xFracModeBitsIntra will not affect the mode for chroma that may have already been pre-estimated.
        uint64_t fracModeBits = xFracModeBitsIntraLuma( pu );

        //restore ctx
        m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::IntraLumaMpmFlag(), intra_ctx_size), ctxStartIntraCtx);

        double cost = (double) minSadHad + (double) fracModeBits * sqrtLambdaForFirstPass;
//        DTRACE(g_trace_ctx, D_INTRA_COST, "IntraMRL: %u, %llu, %f (%d)\n", minSadHad, fracModeBits, cost, pu.intraDir[0]);

        int insertPos = -1;
        updateCandList( ModeInfo( false, false, multiRefIdx, NOT_INTRA_SUBPARTITIONS, pu.intraDir[0] ), cost, RdModeList,  CandCostList, numModesForFullRD, &insertPos );
        updateCandList( ModeInfo( false, false, multiRefIdx, NOT_INTRA_SUBPARTITIONS, pu.intraDir[0] ), (double)minSadHad, HadModeList, CandHadList,  numHadCand );
        m_SortedPelUnitBufs->insert(insertPos, (int)RdModeList.size());
      }
    }
    pu.multiRefIdx = 0;
  }

  if (testMip)
  {
    cu.mipFlag = true;
    pu.multiRefIdx = 0;

    double mipHadCost[MAX_NUM_MIP_MODE] = { MAX_DOUBLE };

    initIntraPatternChType(cu, pu.Y());
    initIntraMip( pu );

    const int transpOff    = getNumModesMip( pu.Y() );
    const int numModesFull = (transpOff << 1);
    for( uint32_t uiModeFull = 0; uiModeFull < numModesFull; uiModeFull++ )
    {
      const bool     isTransposed = (uiModeFull >= transpOff ? true : false);
      const uint32_t uiMode       = (isTransposed ? uiModeFull - transpOff : uiModeFull);

      pu.mipTransposedFlag = isTransposed;
      pu.intraDir[CH_L] = uiMode;
      distParam.cur.buf = piPred.buf = m_SortedPelUnitBufs->getTestBuf().Y().buf;
      predIntraMip(piPred, pu);

      // Use the min between SAD and HAD as the cost criterion
      // SAD is scaled by 2 to align with the scaling of HAD
      Distortion minSadHad = distParam.distFunc(distParam);

      uint64_t fracModeBits = xFracModeBitsIntraLuma( pu );

      //restore ctx
      m_CABACEstimator->getCtx() = SubCtx(CtxSet(Ctx::IntraLumaMpmFlag(), intra_ctx_size), ctxStartIntraCtx);

      double cost = double(minSadHad) + double(fracModeBits) * sqrtLambdaForFirstPass;
      mipHadCost[uiModeFull] = cost;
      DTRACE(g_trace_ctx, D_INTRA_COST, "IntraMIP: %u, %llu, %f (%d)\n", minSadHad, fracModeBits, cost, uiModeFull);

      int insertPos = -1;
      updateCandList( ModeInfo( true, isTransposed, 0, NOT_INTRA_SUBPARTITIONS, pu.intraDir[0] ), cost, RdModeList,  CandCostList, numModesForFullRD+1, &insertPos );
      updateCandList( ModeInfo( true, isTransposed, 0, NOT_INTRA_SUBPARTITIONS, pu.intraDir[0] ), 0.8*(double)minSadHad, HadModeList, CandHadList,  numHadCand );
      m_SortedPelUnitBufs->insert(insertPos, (int)RdModeList.size());
    }

    const double thresholdHadCost = 1.0 + 1.4 / sqrt((double)(pu.lwidth()*pu.lheight()));
    xReduceHadCandList(RdModeList, CandCostList, *m_SortedPelUnitBufs, numModesForFullRD, thresholdHadCost, mipHadCost, pu, fastMip);
  }

  if( m_pcEncCfg->m_bFastUDIUseMPMEnabled )
  {
    const int numMPMs = NUM_MOST_PROBABLE_MODES;
    unsigned  intraMpms[numMPMs];

    pu.multiRefIdx = 0;

    const int numCand = PU::getIntraMPMs( pu, intraMpms );
    ModeInfo mostProbableMode(false, false, 0, NOT_INTRA_SUBPARTITIONS, 0);

    for( int j = 0; j < numCand; j++ )
    {
      bool mostProbableModeIncluded = false;
      mostProbableMode.modeId = intraMpms[j];

      for( int i = 0; i < numModesForFullRD; i++ )
      {
        mostProbableModeIncluded |= ( mostProbableMode == RdModeList[i] );
      }
      if( !mostProbableModeIncluded )
      {
        numModesForFullRD++;
        RdModeList.push_back( mostProbableMode );
        CandCostList.push_back(0);
      }
    }
  }
}

bool IntraSearch::estIntraPredLumaQT(CodingUnit &cu, Partitioner &partitioner, double bestCost)
{
  CodingStructure       &cs           = *cu.cs;
  const int             width         = partitioner.currArea().lwidth();
  const int             height        = partitioner.currArea().lheight();

  //===== loop over partitions =====

  const TempCtx ctxStart           ( m_CtxCache, m_CABACEstimator->getCtx() );
  CHECK( !cu.pu, "CU has no PUs" );

  // variables for saving fast intra modes scan results across multiple LFNST passes
  double costInterCU = xFindInterCUCost( cu );

  auto &pu = *cu.pu;
  bool validReturn = false;

  CHECK(pu.cu != &cu, "PU is not contained in the CU");

  //===== determine set of modes to be tested (using prediction signal only) =====
  int numModesAvailable = NUM_LUMA_MODE; // total number of Intra modes
  static_vector<ModeInfo, FAST_UDI_MAX_RDMODE_NUM> RdModeList;
  static_vector<ModeInfo, FAST_UDI_MAX_RDMODE_NUM> HadModeList;
  static_vector<double, FAST_UDI_MAX_RDMODE_NUM> CandCostList;
  static_vector<double, FAST_UDI_MAX_RDMODE_NUM> CandHadList;

  int numModesForFullRD = g_aucIntraModeNumFast_UseMPM_2D[Log2(width) - MIN_CU_LOG2][Log2(height) - MIN_CU_LOG2];

#if INTRA_FULL_SEARCH
  numModesForFullRD = numModesAvailable;
#endif
  const SPS& sps = *cu.cs->sps;
  const bool mipAllowed = sps.MIP && pu.lwidth() <= sps.getMaxTbSize() && pu.lheight() <= sps.getMaxTbSize() && ((cu.lfnstIdx == 0) || allowLfnstWithMip(cu.pu->lumaSize()));
  const int SizeThr = 8>>std::max(0,m_pcEncCfg->m_useFastMIP-2);
  const bool testMip    = mipAllowed && (cu.lwidth() <= (SizeThr * cu.lheight()) && cu.lheight() <= (SizeThr * cu.lwidth())) && (cu.lwidth() <= MIP_MAX_WIDTH && cu.lheight() <= MIP_MAX_HEIGHT);

  xEstimateLumaRdModeList(numModesForFullRD, RdModeList, HadModeList, CandCostList, CandHadList, cu, testMip);

  CHECK( (size_t)numModesForFullRD != RdModeList.size(), "Inconsistent state!" );

  // after this point, don't use numModesForFullRD
  if( m_pcEncCfg->m_usePbIntraFast && !cs.slice->isIntra() && RdModeList.size() < numModesAvailable && !cs.slice->disableSATDForRd )
  {
    double pbintraRatio = PBINTRA_RATIO;
    int maxSize = -1;
    ModeInfo bestMipMode;
    int bestMipIdx = -1;
    for( int idx = 0; idx < RdModeList.size(); idx++ )
    {
      if( RdModeList[idx].mipFlg )
      {
        bestMipMode = RdModeList[idx];
        bestMipIdx = idx;
        break;
      }
    }
    const int numHadCand = 3;
    for (int k = numHadCand - 1; k >= 0; k--)
    {
      if (CandHadList.size() < (k + 1) || CandHadList[k] > cs.interHad * pbintraRatio) { maxSize = k; }
    }
    if (maxSize > 0)
    {
      RdModeList.resize(std::min<size_t>(RdModeList.size(), maxSize));
      if( bestMipIdx >= 0 )
      {
        if( RdModeList.size() <= bestMipIdx )
        {
          RdModeList.push_back(bestMipMode);
          m_SortedPelUnitBufs->swap( maxSize, bestMipIdx );
        }
      }
    }
    if (maxSize == 0)
    {
      cs.dist = MAX_DISTORTION;
      cs.interHad = 0;
      return false;
    }
  }

  //===== check modes (using r-d costs) =====
  ModeInfo       uiBestPUMode;

  CodingStructure *csTemp = m_pTempCS[Log2(cu.lwidth())][Log2(cu.lheight())];
  CodingStructure *csBest = m_pBestCS[Log2(cu.lwidth())][Log2(cu.lheight())];

  csTemp->slice   = csBest->slice   = cs.slice;
  csTemp->picture = csBest->picture = cs.picture;
  csTemp->initStructData();
  csBest->initStructData();

  int bestLfnstIdx = 0;

  for (int mode = 0; mode < (int)RdModeList.size(); mode++)
  {
    // set CU/PU to luma prediction mode
    ModeInfo testMode;
    {
      testMode              = RdModeList[mode];
      cu.bdpcmMode          = 0;
      cu.ispMode            = testMode.ispMod;
      cu.mipFlag            = testMode.mipFlg;
      pu.mipTransposedFlag  = testMode.mipTrFlg;
      pu.multiRefIdx        = testMode.mRefId;
      pu.intraDir[CH_L]     = testMode.modeId;

      CHECK(cu.mipFlag && pu.multiRefIdx, "Error: combination of MIP and MRL not supported");
      CHECK(pu.multiRefIdx && (pu.intraDir[0] == PLANAR_IDX), "Error: combination of MRL and Planar mode not supported");
      CHECK(cu.ispMode && cu.mipFlag, "Error: combination of ISP and MIP not supported");
      CHECK(cu.ispMode && pu.multiRefIdx, "Error: combination of ISP and MRL not supported");
    }

    // determine residual for partition
    cs.initSubStructure( *csTemp, partitioner.chType, cs.area, true );

    xIntraCodingLumaQT(*csTemp, partitioner, m_SortedPelUnitBufs->getBufFromSortedList(mode), bestCost);

    DTRACE(g_trace_ctx, D_INTRA_COST, "IntraCost T [x=%d,y=%d,w=%d,h=%d] %f (%d,%d,%d,%d,%d,%d) \n", cu.blocks[0].x,
      cu.blocks[0].y, width, height, csTemp->cost, testMode.modeId, testMode.ispMod,
      pu.multiRefIdx, cu.mipFlag, cu.lfnstIdx, cu.mtsFlag);


    // check r-d cost
    if( csTemp->cost < csBest->cost )
    {
      validReturn = true;
      std::swap( csTemp, csBest );
      uiBestPUMode = testMode;
      bestLfnstIdx = csBest->cus[0]->lfnstIdx;
    }

    // reset context models
    m_CABACEstimator->getCtx() = ctxStart;

    csTemp->releaseIntermediateData();

    if( m_pcEncCfg->m_fastLocalDualTreeMode  && cu.isConsIntra() && !cu.slice->isIntra() && csBest->cost != MAX_DOUBLE && costInterCU != COST_UNKNOWN && mode >= 0 )
    {
      if( m_pcEncCfg->m_fastLocalDualTreeMode == 2 )
      {
        //Note: only try one intra mode, which is especially useful to reduce EncT for LDB case (around 4%)
        break;
      }
      else
      {
        if( csBest->cost > costInterCU * 1.5 )
        {
          break;
        }
      }
    }
  } // Mode loop

  if( validReturn )
  {
    cs.useSubStructure( *csBest, partitioner.chType, TREE_D, pu.singleChan( CH_L ), true );
    const ReshapeData& reshapeData = cs.picture->reshapeData;
    if( cs.picHeader->lmcsEnabled && reshapeData.getCTUFlag() )
    {
      cs.getRspRecoBuf().copyFrom( csBest->getRspRecoBuf());
    }

    //=== update PU data ====
    cu.lfnstIdx           = bestLfnstIdx;
    cu.ispMode            = uiBestPUMode.ispMod;
    cu.mipFlag            = uiBestPUMode.mipFlg;
    pu.mipTransposedFlag  = uiBestPUMode.mipTrFlg;
    pu.multiRefIdx        = uiBestPUMode.mRefId;
    pu.intraDir[ CH_L ]   = uiBestPUMode.modeId;
  }
  else
  {
    THROW("fix this");
  }

  csBest->releaseIntermediateData();

  return validReturn;
}

void IntraSearch::estIntraPredChromaQT( CodingUnit &cu, Partitioner &partitioner )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 0, g_timeProfiler, P_INTRA_CHROMA, cu.cs, CH_C );
  const ChromaFormat format   = cu.chromaFormat;
  const uint32_t    numberValidComponents = getNumberValidComponents(format);
  CodingStructure &cs = *cu.cs;
  const TempCtx ctxStart  ( m_CtxCache, m_CABACEstimator->getCtx() );

  cs.setDecomp( cs.area.Cb(), false );

  auto &pu = *cu.pu;

  uint32_t   uiBestMode = 0;
  Distortion uiBestDist = 0;
  double     dBestCost  = MAX_DOUBLE;

  //----- init mode list ----
  {
    uint32_t  uiMinMode = 0;
    uint32_t  uiMaxMode = NUM_CHROMA_MODE;

    //----- check chroma modes -----
    uint32_t chromaCandModes[ NUM_CHROMA_MODE ];
    PU::getIntraChromaCandModes( pu, chromaCandModes );

    // create a temporary CS
    CodingStructure &saveCS = *m_pSaveCS[0];
    saveCS.pcv      = cs.pcv;
    saveCS.picture  = cs.picture;
    saveCS.area.repositionTo( cs.area );
    saveCS.clearTUs();

      if( !cu.isSepTree() && cu.ispMode )
      {
        saveCS.clearCUs();
        saveCS.clearPUs();
      }

    if( cu.isSepTree() )
    {
      if( partitioner.canSplit( TU_MAX_TR_SPLIT, cs ) )
      {
        partitioner.splitCurrArea( TU_MAX_TR_SPLIT, cs );

        do
        {
          cs.addTU( CS::getArea( cs, partitioner.currArea(), partitioner.chType, partitioner.treeType ), partitioner.chType, &cu ).depth = partitioner.currTrDepth;
        } while( partitioner.nextPart( cs ) );

        partitioner.exitCurrSplit();
      }
      else
        cs.addTU( CS::getArea( cs, partitioner.currArea(), partitioner.chType, partitioner.treeType ), partitioner.chType, &cu );
    }

    std::vector<TransformUnit*> orgTUs;

    // create a store for the TUs
    for( const auto &ptu : cs.tus )
    {
      // for split TUs in HEVC, add the TUs without Chroma parts for correct setting of Cbfs
      if( /*lumaUsesISP ||*/ pu.contains( *ptu, CH_C ) )
      {
        saveCS.addTU( *ptu, partitioner.chType, nullptr );
        orgTUs.push_back( ptu );
      }
    }

    // SATD pre-selecting.
    int     satdModeList  [NUM_CHROMA_MODE] = { 0 };
    int64_t satdSortedCost[NUM_CHROMA_MODE] = { 0 };
    bool    modeDisable[NUM_INTRA_MODE + 1] = { false }; // use intra mode idx to check whether enable

    CodingStructure& cs = *(pu.cs);
    CompArea areaCb = pu.Cb();
    CompArea areaCr = pu.Cr();
    CPelBuf orgCb  = cs.getOrgBuf (COMP_Cb);
    PelBuf predCb  = cs.getPredBuf(COMP_Cb);
    CPelBuf orgCr  = cs.getOrgBuf (COMP_Cr);
    PelBuf predCr  = cs.getPredBuf(COMP_Cr);

    DistParam distParamSadCb  = m_pcRdCost->setDistParam( orgCb, predCb, pu.cs->sps->bitDepths[ CH_C ], DF_SAD);
    DistParam distParamSatdCb = m_pcRdCost->setDistParam( orgCb, predCb, pu.cs->sps->bitDepths[ CH_C ], DF_HAD);
    DistParam distParamSadCr  = m_pcRdCost->setDistParam( orgCr, predCr, pu.cs->sps->bitDepths[ CH_C ], DF_SAD);
    DistParam distParamSatdCr = m_pcRdCost->setDistParam( orgCr, predCr, pu.cs->sps->bitDepths[ CH_C ], DF_HAD);

    pu.intraDir[1] = MDLM_L_IDX; // temporary assigned, just to indicate this is a MDLM mode. for luma down-sampling operation.

    initIntraPatternChType(cu, pu.Cb());
    initIntraPatternChType(cu, pu.Cr());
    loadLMLumaRecPels(pu, pu.Cb());

    for (int idx = uiMinMode; idx < uiMaxMode; idx++)
    {
      int mode = chromaCandModes[idx];
      satdModeList[idx] = mode;
      if (PU::isLMCMode(mode) && !PU::isLMCModeEnabled(pu, mode))
      {
        continue;
      }
      if ((mode == LM_CHROMA_IDX) || (mode == PLANAR_IDX) || (mode == DM_CHROMA_IDX)) // only pre-check regular modes and MDLM modes, not including DM ,Planar, and LM
      {
        continue;
      }

      pu.intraDir[1]    = mode; // temporary assigned, for SATD checking.

      const bool isLMCMode = PU::isLMCMode(mode);
      if( isLMCMode )
      {
        predIntraChromaLM(COMP_Cb, predCb, pu, areaCb, mode);
      }
      else
      {
        initPredIntraParams(pu, pu.Cb(), *cs.sps);
        predIntraAng(COMP_Cb, predCb, pu);
      }
      int64_t sadCb = distParamSadCb.distFunc(distParamSadCb) * 2;
      int64_t satdCb = distParamSatdCb.distFunc(distParamSatdCb);
      int64_t sad = std::min(sadCb, satdCb);

      if( isLMCMode )
      {
        predIntraChromaLM(COMP_Cr, predCr, pu, areaCr, mode);
      }
      else
      {
        initPredIntraParams(pu, pu.Cr(), *cs.sps);
        predIntraAng(COMP_Cr, predCr, pu);
      }
      int64_t sadCr = distParamSadCr.distFunc(distParamSadCr) * 2;
      int64_t satdCr = distParamSatdCr.distFunc(distParamSatdCr);
      sad += std::min(sadCr, satdCr);
      satdSortedCost[idx] = sad;
    }

    // sort the mode based on the cost from small to large.
    for (int i = uiMinMode; i <= uiMaxMode - 1; i++)
    {
      for (int j = i + 1; j <= uiMaxMode - 1; j++)
      {
        if (satdSortedCost[j] < satdSortedCost[i])
        {
          std::swap( satdModeList[i],   satdModeList[j]);
          std::swap( satdSortedCost[i], satdSortedCost[j]);
        }
      }
    }

    int reducedModeNumber = 2; // reduce the number of chroma modes
    for (int i = 0; i < reducedModeNumber; i++)
    {
      modeDisable[satdModeList[uiMaxMode - 1 - i]] = true; // disable the last reducedModeNumber modes
    }

    int bestLfnstIdx = 0;
    // save the dist
    Distortion baseDist = cs.dist;

    for (uint32_t uiMode = uiMinMode; uiMode < uiMaxMode; uiMode++)
    {
      const int chromaIntraMode = chromaCandModes[uiMode];
      if( PU::isLMCMode( chromaIntraMode ) && ! PU::isLMCModeEnabled( pu, chromaIntraMode ) )
      {
        continue;
      }
      if( modeDisable[chromaIntraMode] && PU::isLMCModeEnabled(pu, chromaIntraMode)) // when CCLM is disable, then MDLM is disable. not use satd checking
      {
        continue;
      }
      cs.setDecomp( pu.Cb(), false );
      cs.dist = baseDist;
      //----- restore context models -----
      m_CABACEstimator->getCtx() = ctxStart;

      //----- chroma coding -----
      pu.intraDir[1] = chromaIntraMode;

      xIntraChromaCodingQT( cs, partitioner );

      if (cs.sps->transformSkip)
      {
        m_CABACEstimator->getCtx() = ctxStart;
      }

      uint64_t fracBits   = xGetIntraFracBitsQT( cs, partitioner, false );
      Distortion uiDist = cs.dist;
      double    dCost   = m_pcRdCost->calcRdCost( fracBits, uiDist - baseDist );

      //----- compare -----
      if( dCost < dBestCost )
      {
        for( uint32_t i = getFirstComponentOfChannel( CH_C ); i < numberValidComponents; i++ )
        {
          const CompArea& area = pu.blocks[i];
          saveCS.getRecoBuf     ( area ).copyFrom( cs.getRecoBuf   ( area ) );
          cs.picture->getRecoBuf( area ).copyFrom( cs.getRecoBuf   ( area ) );
          for( uint32_t j = 0; j < saveCS.tus.size(); j++ )
          {
            saveCS.tus[j]->copyComponentFrom( *orgTUs[j], area.compID );
          }
        }
        dBestCost    = dCost;
        uiBestDist   = uiDist;
        uiBestMode   = chromaIntraMode;
        bestLfnstIdx = cu.lfnstIdx;
      }
    }
    cu.lfnstIdx = bestLfnstIdx;

    for( uint32_t i = getFirstComponentOfChannel( CH_C ); i < numberValidComponents; i++ )
    {
      const CompArea& area = pu.blocks[i];

      cs.getRecoBuf         ( area ).copyFrom( saveCS.getRecoBuf( area ) );
      cs.picture->getRecoBuf( area ).copyFrom( cs.getRecoBuf    ( area ) );

      for( uint32_t j = 0; j < saveCS.tus.size(); j++ )
      {
        orgTUs[ j ]->copyComponentFrom( *saveCS.tus[ j ], area.compID );
      }
    }
  }

  pu.intraDir[1] = uiBestMode;
  cs.dist        = uiBestDist;

  //----- restore context models -----
  m_CABACEstimator->getCtx() = ctxStart;
}

void IntraSearch::saveCuAreaCostInSCIPU( Area area, double cost )
{
  if( m_numCuInSCIPU < NUM_INTER_CU_INFO_SAVE )
  {
    m_cuAreaInSCIPU[m_numCuInSCIPU] = area;
    m_cuCostInSCIPU[m_numCuInSCIPU] = cost;
    m_numCuInSCIPU++;
  }
}

void IntraSearch::initCuAreaCostInSCIPU()
{
  for( int i = 0; i < NUM_INTER_CU_INFO_SAVE; i++ )
  {
    m_cuAreaInSCIPU[i] = Area();
    m_cuCostInSCIPU[i] = 0;
  }
  m_numCuInSCIPU = 0;
}
// -------------------------------------------------------------------------------------------------------------------
// Intra search
// -------------------------------------------------------------------------------------------------------------------

void IntraSearch::xEncIntraHeader( CodingStructure &cs, Partitioner &partitioner, const bool luma )
{
  CodingUnit &cu = *cs.getCU( partitioner.chType, partitioner.treeType );

  if (luma)
  {
    bool isFirst = partitioner.currArea().lumaPos() == cs.area.lumaPos();

    // CU header
    if( isFirst )
    {
      if ((!cs.slice->isIntra() || cs.slice->sps->IBC || cs.slice->sps->PLT) && cu.Y().valid())
      {
        m_CABACEstimator->pred_mode   ( cu );
      }
      m_CABACEstimator->bdpcm_mode  ( cu, ComponentID(partitioner.chType) );
    }

    // luma prediction mode
    if (isFirst)
    {
      if ( !cu.Y().valid())
      {
        m_CABACEstimator->pred_mode( cu );
      }
      m_CABACEstimator->intra_luma_pred_mode( *cu.pu );
    }
  }
  else //  if (chroma)
  {
    bool isFirst = partitioner.currArea().Cb().valid() && partitioner.currArea().chromaPos() == cs.area.chromaPos();

    if( isFirst )
    {
      m_CABACEstimator->intra_chroma_pred_mode(  *cu.pu );
    }
  }
}

void IntraSearch::xEncSubdivCbfQT( CodingStructure &cs, Partitioner &partitioner, const bool luma )
{
  const UnitArea& currArea = partitioner.currArea();
  TransformUnit  &currTU   = *cs.getTU( currArea.blocks[partitioner.chType], partitioner.chType );
  CodingUnit     &currCU   = *currTU.cu;
  const uint32_t currDepth = partitioner.currTrDepth;


  //===== Cbfs =====
  if (luma)
  {
    bool previousCbf       = false;
    bool lastCbfIsInferred = false;
    if( !lastCbfIsInferred )
    {
      m_CABACEstimator->cbf_comp( currCU, TU::getCbfAtDepth( currTU, COMP_Y, currDepth ), currTU.Y(), currTU.depth, previousCbf, currCU.ispMode );
    }
  }
  else  //if( chroma )
  {
    const uint32_t numberValidComponents = getNumberValidComponents(currArea.chromaFormat);
    const uint32_t cbfDepth = currDepth;

    for (uint32_t ch = COMP_Cb; ch < numberValidComponents; ch++)
    {
      const ComponentID compID = ComponentID(ch);

      if( currDepth == 0 || TU::getCbfAtDepth( currTU, compID, currDepth - 1 ) )
      {
        const bool prevCbf = ( compID == COMP_Cr ? TU::getCbfAtDepth( currTU, COMP_Cb, currDepth ) : false );
        m_CABACEstimator->cbf_comp( currCU, TU::getCbfAtDepth( currTU, compID, currDepth ), currArea.blocks[compID], cbfDepth, prevCbf );
      }
    }
  }
}

void IntraSearch::xEncCoeffQT( CodingStructure &cs, Partitioner &partitioner, const ComponentID compID, CUCtx *cuCtx )
{
  const UnitArea& currArea  = partitioner.currArea();

  TransformUnit& currTU     = *cs.getTU( currArea.blocks[partitioner.chType], partitioner.chType );
  uint32_t   currDepth      = partitioner.currTrDepth;
  const bool subdiv         = currTU.depth > currDepth;

  if (subdiv)
  {
    if (partitioner.canSplit(TU_MAX_TR_SPLIT, cs))
    {
      partitioner.splitCurrArea(TU_MAX_TR_SPLIT, cs);
    }
    else
      THROW("Implicit TU split not available!");

    do
    {
      xEncCoeffQT( cs, partitioner, compID );
    } while( partitioner.nextPart( cs ) );

    partitioner.exitCurrSplit();
  }
  else

  if( currArea.blocks[compID].valid() )
  {
    if( compID == COMP_Cr )
    {
      const int cbfMask = ( TU::getCbf( currTU, COMP_Cb ) ? 2 : 0 ) + ( TU::getCbf( currTU, COMP_Cr ) ? 1 : 0 );
      m_CABACEstimator->joint_cb_cr( currTU, cbfMask );
    }
    if( TU::getCbf( currTU, compID ) )
    {
      if( isLuma(compID) )
      {
        m_CABACEstimator->residual_coding( currTU, compID, cuCtx );
        m_CABACEstimator->mts_idx( *currTU.cu, cuCtx );
      }
      else
        m_CABACEstimator->residual_coding( currTU, compID );
    }
  }
}

uint64_t IntraSearch::xGetIntraFracBitsQT( CodingStructure &cs, Partitioner &partitioner, const bool luma, CUCtx *cuCtx )
{
  m_CABACEstimator->resetBits();

  xEncIntraHeader( cs, partitioner, luma );
  xEncSubdivCbfQT( cs, partitioner, luma );

  if( luma )
  {
    xEncCoeffQT( cs, partitioner, COMP_Y, cuCtx );

    CodingUnit &cu = *cs.cus[0];
    if( cuCtx )
    {
      m_CABACEstimator->residual_lfnst_mode( cu, *cuCtx );
    }
  }
  else
  {
    xEncCoeffQT( cs, partitioner, COMP_Cb );
    xEncCoeffQT( cs, partitioner, COMP_Cr );
  }

  uint64_t fracBits = m_CABACEstimator->getEstFracBits();
  return fracBits;
}

uint64_t IntraSearch::xGetIntraFracBitsQTChroma(const TransformUnit& currTU, const ComponentID compID, CUCtx *cuCtx)
{
  m_CABACEstimator->resetBits();

  if ( currTU.jointCbCr )
  {
    const int cbfMask = ( TU::getCbf( currTU, COMP_Cb ) ? 2 : 0 ) + ( TU::getCbf( currTU, COMP_Cr ) ? 1 : 0 );
    m_CABACEstimator->cbf_comp( *currTU.cu, cbfMask>>1, currTU.blocks[ COMP_Cb ], currTU.depth, false );
    m_CABACEstimator->cbf_comp( *currTU.cu, cbfMask &1, currTU.blocks[ COMP_Cr ], currTU.depth, cbfMask>>1 );
    if( cbfMask )
      m_CABACEstimator->joint_cb_cr( currTU, cbfMask );
    if (cbfMask >> 1)
      m_CABACEstimator->residual_coding( currTU, COMP_Cb, cuCtx );
    if (cbfMask & 1)
      m_CABACEstimator->residual_coding( currTU, COMP_Cr, cuCtx );
  }
  else
  {
    if ( compID == COMP_Cb )
      m_CABACEstimator->cbf_comp( *currTU.cu, TU::getCbf( currTU, compID ), currTU.blocks[ compID ], currTU.depth, false );
    else
    {
      const bool cbCbf    = TU::getCbf( currTU, COMP_Cb );
      const bool crCbf    = TU::getCbf( currTU, compID );
      const int  cbfMask  = ( cbCbf ? 2 : 0 ) + ( crCbf ? 1 : 0 );
      m_CABACEstimator->cbf_comp( *currTU.cu, crCbf, currTU.blocks[ compID ], currTU.depth, cbCbf );
      m_CABACEstimator->joint_cb_cr( currTU, cbfMask );
    }
  }

  if( !currTU.jointCbCr && TU::getCbf( currTU, compID ) )
  {
    m_CABACEstimator->residual_coding( currTU, compID, cuCtx );
  }

  uint64_t fracBits = m_CABACEstimator->getEstFracBits();
  return fracBits;
}

void IntraSearch::xIntraCodingTUBlock(TransformUnit &tu, const ComponentID compID, const bool checkCrossCPrediction, Distortion &ruiDist, uint32_t *numSig, PelUnitBuf *predBuf, const bool loadTr)
{
  if (!tu.blocks[compID].valid())
  {
    return;
  }

  CodingStructure &cs             = *tu.cs;
  const CompArea      &area       = tu.blocks[compID];
  const SPS           &sps        = *cs.sps;
  const ReshapeData&  reshapeData = cs.picture->reshapeData;

  const ChannelType    chType     = toChannelType(compID);
  const int            bitDepth   = sps.bitDepths[chType];

  CPelBuf        piOrg            = cs.getOrgBuf    (compID);
  PelBuf         piPred           = cs.getPredBuf   (compID);
  PelBuf         piResi           = cs.getResiBuf   (compID);
  PelBuf         piReco           = cs.getRecoBuf   (compID);

  const PredictionUnit &pu        = *cs.getPU(area.pos(), chType);

  //===== init availability pattern =====
  CHECK( tu.jointCbCr && compID == COMP_Cr, "wrong combination of compID and jointCbCr" );
  bool jointCbCr = tu.jointCbCr && compID == COMP_Cb;

  if ( isLuma(compID) )
  {
    bool predRegDiffFromTB = CU::isPredRegDiffFromTB(*tu.cu, compID);
    bool firstTBInPredReg = CU::isFirstTBInPredReg(*tu.cu, compID, area);
    CompArea areaPredReg(COMP_Y, tu.chromaFormat, area);
    {
      initIntraPatternChType(*tu.cu, area);
    }

    //===== get prediction signal =====
    if (predRegDiffFromTB)
    {
      if (firstTBInPredReg)
      {
        PelBuf piPredReg = cs.getPredBuf(areaPredReg);
        predIntraAng(compID, piPredReg, pu);
      }
    }
    else
    {
      if( predBuf )
      {
        piPred.copyFrom( predBuf->Y() );
      }
      else if( PU::isMIP( pu, CH_L ) )
      {
        initIntraMip( pu );
        predIntraMip( piPred, pu );
      }
      else
      {
        predIntraAng(compID, piPred, pu);
      }
    }
  }
  DTRACE( g_trace_ctx, D_PRED, "@(%4d,%4d) [%2dx%2d] IMode=%d\n", tu.lx(), tu.ly(), tu.lwidth(), tu.lheight(), PU::getFinalIntraMode(pu, chType) );
  const Slice &slice = *cs.slice;
  bool flag = cs.picHeader->lmcsEnabled && (slice.isIntra() || (!slice.isIntra() && reshapeData.getCTUFlag()));

  if (isLuma(compID))
  {
    //===== get residual signal =====
    if (cs.picHeader->lmcsEnabled && reshapeData.getCTUFlag() )
    {
      piResi.subtract( cs.getRspOrgBuf(), piPred);
    }
    else
    {
      piResi.subtract( piOrg, piPred );
    }
  }

  //===== transform and quantization =====
  //--- init rate estimation arrays for RDOQ ---
  //--- transform and quantization           ---
  TCoeff uiAbsSum = 0;
  const QpParam cQP(tu, compID);

  m_pcTrQuant->selectLambda(compID);

  flag =flag && (tu.blocks[compID].width*tu.blocks[compID].height > 4);
  if (flag && isChroma(compID) && cs.picHeader->lmcsChromaResidualScale )
  {
    int cResScaleInv = tu.chromaAdj;
    double cRescale = (double)(1 << CSCALE_FP_PREC) / (double)cResScaleInv;
    m_pcTrQuant->scaleLambda( 1.0/(cRescale*cRescale) );
  }

  CPelBuf         crOrg  = cs.getOrgBuf  ( COMP_Cr );
  PelBuf          crPred = cs.getPredBuf ( COMP_Cr );
  PelBuf          crResi = cs.getResiBuf ( COMP_Cr );
  PelBuf          crReco = cs.getRecoBuf ( COMP_Cr );

  if ( jointCbCr )
  {
    // Lambda is loosened for the joint mode with respect to single modes as the same residual is used for both chroma blocks
    const int    absIct = abs( TU::getICTMode(tu) );
    const double lfact  = ( absIct == 1 || absIct == 3 ? 0.8 : 0.5 );
    m_pcTrQuant->scaleLambda( lfact );
  }
  if ( sps.jointCbCr && isChroma(compID) && (tu.cu->cs->slice->sliceQp > 18) )
  {
    m_pcTrQuant->scaleLambda( 1.3 );
  }

  if( isLuma(compID) )
  {
    m_pcTrQuant->transformNxN(tu, compID, cQP, uiAbsSum, m_CABACEstimator->getCtx(), loadTr);

    DTRACE( g_trace_ctx, D_TU_ABS_SUM, "%d: comp=%d, abssum=%d\n", DTRACE_GET_COUNTER( g_trace_ctx, D_TU_ABS_SUM ), compID, uiAbsSum );

    //--- inverse transform ---
    if (uiAbsSum > 0)
    {
      m_pcTrQuant->invTransformNxN(tu, compID, piResi, cQP);
    }
    else
    {
      piResi.fill(0);
    }
  }
  else // chroma
  {
    int         codedCbfMask  = 0;
    ComponentID codeCompId    = (tu.jointCbCr ? (tu.jointCbCr >> 1 ? COMP_Cb : COMP_Cr) : compID);
    const QpParam qpCbCr(tu, codeCompId);

    if( tu.jointCbCr )
    {
      ComponentID otherCompId = ( codeCompId==COMP_Cr ? COMP_Cb : COMP_Cr );
      tu.getCoeffs( otherCompId ).fill(0); // do we need that?
      TU::setCbfAtDepth (tu, otherCompId, tu.depth, false );
    }
    PelBuf& codeResi = ( codeCompId == COMP_Cr ? crResi : piResi );
    uiAbsSum = 0;
    m_pcTrQuant->transformNxN(tu, codeCompId, qpCbCr, uiAbsSum, m_CABACEstimator->getCtx(), loadTr);
    DTRACE( g_trace_ctx, D_TU_ABS_SUM, "%d: comp=%d, abssum=%d\n", DTRACE_GET_COUNTER( g_trace_ctx, D_TU_ABS_SUM ), codeCompId, uiAbsSum );
    if( uiAbsSum > 0 )
    {
      m_pcTrQuant->invTransformNxN(tu, codeCompId, codeResi, qpCbCr);
      codedCbfMask += ( codeCompId == COMP_Cb ? 2 : 1 );
    }
    else
    {
      codeResi.fill(0);
    }

    if( tu.jointCbCr )
    {
      if( tu.jointCbCr == 3 && codedCbfMask == 2 )
      {
        codedCbfMask = 3;
        TU::setCbfAtDepth (tu, COMP_Cr, tu.depth, true );
      }
      if( tu.jointCbCr != codedCbfMask )
      {
        ruiDist = MAX_DISTORTION;
        return;
      }
      m_pcTrQuant->invTransformICT( tu, piResi, crResi );
      uiAbsSum = codedCbfMask;
    }
  }

  //===== reconstruction =====
  if ( flag && uiAbsSum > 0 && isChroma(compID) && cs.picHeader->lmcsChromaResidualScale )
  {
    piResi.scaleSignal(tu.chromaAdj, 0, slice.clpRngs[compID]);

    if( jointCbCr )
    {
      crResi.scaleSignal(tu.chromaAdj, 0, slice.clpRngs[COMP_Cr]);
    }
  }

  piReco.reconstruct(piPred, piResi, cs.slice->clpRngs[ compID ]);
  if( jointCbCr )
  {
    crReco.reconstruct(crPred, crResi, cs.slice->clpRngs[ COMP_Cr ]);
  }


  //===== update distortion =====
  if( (cs.picHeader->lmcsEnabled && reshapeData.getCTUFlag()) || m_pcEncCfg->m_lumaLevelToDeltaQPEnabled )
  {
    const CPelBuf orgLuma = cs.getOrgBuf( cs.area.blocks[COMP_Y] );
    if( compID == COMP_Y && !m_pcEncCfg->m_lumaLevelToDeltaQPEnabled )
    {
      PelBuf tmpRecLuma = cs.getRspRecoBuf();
      tmpRecLuma.rspSignal( piReco, reshapeData.getInvLUT());
      ruiDist += m_pcRdCost->getDistPart(piOrg, tmpRecLuma, sps.bitDepths[toChannelType(compID)], compID, DF_SSE_WTD, &orgLuma);
    }
    else
    {
      ruiDist += m_pcRdCost->getDistPart( piOrg, piReco, bitDepth, compID, DF_SSE_WTD, &orgLuma );
      if( jointCbCr )
      {
        ruiDist += m_pcRdCost->getDistPart( crOrg, crReco, bitDepth, COMP_Cr, DF_SSE_WTD, &orgLuma );
      }
    }
  }
  else
  {
    ruiDist += m_pcRdCost->getDistPart( piOrg, piReco, bitDepth, compID, DF_SSE );
    if( jointCbCr )
    {
      ruiDist += m_pcRdCost->getDistPart( crOrg, crReco, bitDepth, COMP_Cr, DF_SSE );
    }
  }
}

void IntraSearch::xIntraCodingLumaQT( CodingStructure& cs, Partitioner& partitioner, PelUnitBuf* predBuf, const double bestCostSoFar )
{
  PROFILER_SCOPE_AND_STAGE_EXT( 0, g_timeProfiler, P_INTRA_RD_SEARCH_LUMA, &cs, partitioner.chType );
  const UnitArea& currArea  = partitioner.currArea();
  uint32_t        currDepth = partitioner.currTrDepth;

  TransformUnit& tu = cs.addTU( CS::getArea( cs, currArea, partitioner.chType, partitioner.treeType ), partitioner.chType, cs.cus[0] );
  tu.depth = currDepth;

  CHECK( !tu.Y().valid(), "Invalid TU" );

  Distortion singleDistLuma = 0;
  uint32_t   numSig         = 0;
  const SPS &sps            = *cs.sps;
  CodingUnit &cu            = *cs.cus[0];
  bool mtsAllowed = CU::isMTSAllowed(cu, COMP_Y);
  uint64_t singleFracBits = 0;
  int endLfnstIdx   = (partitioner.isSepTree(cs) && partitioner.chType == CH_C && (currArea.lwidth() < 8 || currArea.lheight() < 8))
                        || (currArea.lwidth() > sps.getMaxTbSize() || currArea.lheight() > sps.getMaxTbSize()) || !sps.LFNST ? 0 : 2;
  
  if (cu.mipFlag && !allowLfnstWithMip(cu.pu->lumaSize()))
  {
    endLfnstIdx = 0;
  }
  int bestMTS = 0;
  int EndMTS = mtsAllowed ? m_pcEncCfg->m_MTSIntraMaxCand +1 : 0;
  if (endLfnstIdx || EndMTS)
  {
    CUCtx cuCtx;
    cuCtx.isDQPCoded         = true;
    cuCtx.isChromaQpAdjCoded = true;
    cs.cost                  = 0.0;

    double           dSingleCost       = MAX_DOUBLE;
    Distortion       singleDistTmpLuma = 0;
    uint64_t         singleTmpFracBits = 0;
    double           singleCostTmp     = 0;
    const TempCtx    ctxStart(m_CtxCache, m_CABACEstimator->getCtx());
    TempCtx          ctxBest(m_CtxCache);
    CodingStructure &saveCS        = *m_pSaveCS[0];
    TransformUnit *  tmpTU         = nullptr;
    int bestLfnstIdx  = 0;
    int startLfnstIdx = 0;
    // speedUps LFNST
    bool   rapidLFNST   = false;
    bool   rapidDCT     = false;
    double thresholdDCT = 1;
    if (m_pcEncCfg->m_MTS == 2)
    {
      thresholdDCT += 1.4 / sqrt(cu.lwidth() * cu.lheight());
    }

    if (m_pcEncCfg->m_LFNST > 1)
    {
      rapidLFNST = true;
      if (m_pcEncCfg->m_LFNST > 2)
      {
        rapidDCT = true;
        endLfnstIdx = endLfnstIdx ? 1 : 0;
      }
    }

    saveCS.pcv     = cs.pcv;
    saveCS.picture = cs.picture;
    saveCS.area.repositionTo(cs.area);
    saveCS.clearTUs();
    tmpTU = &saveCS.addTU( currArea, partitioner.chType, cs.cus[0] );

    std::vector<TrMode> trModes;
    trModes.push_back(TrMode(0, true)); 
    double dct2Cost = MAX_DOUBLE;
    double trGrpStopThreshold =  1.001;
    double trGrpBestCost      = MAX_DOUBLE;
    if (mtsAllowed)
    {
      if (m_pcEncCfg->m_LFNST )
      {
        uint32_t uiIntraMode = cs.pus[0]->intraDir[partitioner.chType];
        int MTScur = (uiIntraMode < 34) ? MTS_DST7_DCT8 : MTS_DCT8_DST7;
        trModes.push_back(TrMode(2, true));
        trModes.push_back(TrMode(MTScur, true));
        MTScur = (uiIntraMode < 34) ? MTS_DCT8_DST7 : MTS_DST7_DCT8;
        trModes.push_back(TrMode(MTScur, true));
        trModes.push_back(TrMode(MTS_DST7_DST7 + 3, true));
      }
      else 
      {
        for (int i = 2; i < 6; i++)
        {
          trModes.push_back(TrMode(i, true));
        }
      }
    }
    if (EndMTS && !m_pcEncCfg->m_LFNST)
    {
      xPreCheckMTS(tu, &trModes, m_pcEncCfg->m_MTSIntraMaxCand, predBuf);
    }
    bool NStopMTS = true;
    for (int modeId = 0; (modeId <= EndMTS)&&NStopMTS; modeId++)
    {
      if (modeId > 1)
      {
        trGrpBestCost = MAX_DOUBLE;
      }
    for (int lfnstIdx = startLfnstIdx; lfnstIdx <= endLfnstIdx; lfnstIdx++)
    {
      if (lfnstIdx && modeId)
      {
        continue;
      }
      if (mtsAllowed)
      {
        if (!m_pcEncCfg->m_LFNST  && !trModes[modeId].second)
        {
          continue;
        }
        tu.mtsIdx[COMP_Y] = trModes[modeId].first;
      }
      cu.lfnstIdx                          = lfnstIdx;
      cuCtx.lfnstLastScanPos               = false;
      cuCtx.violatesLfnstConstrained[CH_L] = false;
      cuCtx.violatesLfnstConstrained[CH_C] = false;

      if ((lfnstIdx != startLfnstIdx) || (modeId))
      {
        m_CABACEstimator->getCtx() = ctxStart;
      }
      singleDistTmpLuma = 0;
      bool TrLoad = (EndMTS && !m_pcEncCfg->m_LFNST) ? true : false;
      xIntraCodingTUBlock(tu, COMP_Y, false, singleDistTmpLuma, &numSig, predBuf, TrLoad);

      cuCtx.mtsLastScanPos = false;
      //----- determine rate and r-d cost -----
      singleTmpFracBits = xGetIntraFracBitsQT(cs, partitioner, true, &cuCtx);
      if (tu.mtsIdx[COMP_Y] > MTS_SKIP)
      {
        if (!cuCtx.mtsLastScanPos)
        {
          singleCostTmp = MAX_DOUBLE;
        }
        else
        {
          singleCostTmp = m_pcRdCost->calcRdCost(singleTmpFracBits, singleDistTmpLuma);
        }
      }
      else
      {
        singleCostTmp     = m_pcRdCost->calcRdCost(singleTmpFracBits, singleDistTmpLuma);
      }
      if (((EndMTS && (m_pcEncCfg->m_MTS == 2)) || rapidLFNST) && (modeId == 0) && (lfnstIdx == 0))
      {
        if (singleCostTmp > bestCostSoFar * thresholdDCT)
        {
          EndMTS = 0;
          if (rapidDCT)
          {
            endLfnstIdx = 0;   // break the loop but do not cpy best
          }
        }
      }
      if (lfnstIdx && !cuCtx.lfnstLastScanPos && !cu.ispMode)
      {
        bool rootCbfL = false;
        for( uint32_t t = 0; t < getNumberValidTBlocks(*cu.cs->pcv); t++)
        {
          rootCbfL |= tu.cbf[t] != 0;
        }
        if( rapidLFNST && !rootCbfL )
        {
          endLfnstIdx = lfnstIdx; // break the loop
        }
        bool cbfAtZeroDepth = cu.isSepTree()
                                ? rootCbfL
                                : (cs.area.chromaFormat != CHROMA_400
                                   && std::min(cu.firstTU->blocks[1].width, cu.firstTU->blocks[1].height) < 4)
                                    ? TU::getCbfAtDepth(tu, COMP_Y, currDepth)
                                    : rootCbfL;
        if (cbfAtZeroDepth)
        {
          singleCostTmp = MAX_DOUBLE;
        }
      }
      if (singleCostTmp < dSingleCost)
      {
        trGrpBestCost = singleCostTmp;
        dSingleCost    = singleCostTmp;
        singleDistLuma = singleDistTmpLuma;
        singleFracBits = singleTmpFracBits;
        bestLfnstIdx   = lfnstIdx;
        bestMTS        = modeId;
        if ((lfnstIdx == 0) && (modeId == 0) )
        {
          dct2Cost = singleCostTmp;
          if (!TU::getCbfAtDepth(tu, COMP_Y, currDepth))
          {
            if (rapidLFNST)
            {
               endLfnstIdx = 0;   // break the loop but do not cpy best
            } 
            EndMTS = 0;
          }
        }

        if ((bestLfnstIdx != endLfnstIdx) || (bestMTS != EndMTS))
        {
          saveCS.getPredBuf(tu.Y()).copyFrom(cs.getPredBuf(tu.Y()));
          saveCS.getRecoBuf(tu.Y()).copyFrom(cs.getRecoBuf(tu.Y()));

          tmpTU->copyComponentFrom(tu, COMP_Y);

          ctxBest = m_CABACEstimator->getCtx();
        }
      
      }
      else
      {
        if( rapidLFNST )
        {
          endLfnstIdx = lfnstIdx; // break the loop
        }
      }
      }
      if (m_pcEncCfg->m_LFNST && ( m_pcEncCfg->m_MTS==2) && modeId && (modeId != EndMTS))
      {
        NStopMTS = false;
        if (bestMTS || bestLfnstIdx)
        {
          if ((modeId > 1 && (bestMTS == modeId))
            || (modeId == 1))
          {
            NStopMTS = (dct2Cost / trGrpBestCost) < trGrpStopThreshold;
          }
        }
      }
    }
    cu.lfnstIdx = bestLfnstIdx;
    if ((bestLfnstIdx != endLfnstIdx) || (bestMTS != EndMTS))
    {
      cs.getRecoBuf(tu.Y()).copyFrom(saveCS.getRecoBuf(tu.Y()));

      tu.copyComponentFrom(*tmpTU, COMP_Y);

      m_CABACEstimator->getCtx() = ctxBest;
    }

    // otherwise this would've happened in useSubStructure
    cs.picture->getRecoBuf(currArea.Y()).copyFrom(cs.getRecoBuf(currArea.Y()));
  }
  else
  {
    xIntraCodingTUBlock(tu, COMP_Y, false, singleDistLuma, &numSig, predBuf);

    //----- determine rate and r-d cost -----
    singleFracBits = xGetIntraFracBitsQT(cs, partitioner, true);
  }

  cs.dist     += singleDistLuma;
  cs.fracBits += singleFracBits;
  cs.cost      = m_pcRdCost->calcRdCost( cs.fracBits, cs.dist );

  STAT_COUNT_CU_MODES( partitioner.chType == CH_L, g_cuCounters1D[CU_RD_TESTS][0][!cs.slice->isIntra() + cs.slice->depth] );
  STAT_COUNT_CU_MODES( partitioner.chType == CH_L && !cs.slice->isIntra(), g_cuCounters2D[CU_RD_TESTS][Log2( cs.area.lheight() )][Log2( cs.area.lwidth() )] );
}

void IntraSearch::xIntraChromaCodingQT( CodingStructure &cs, Partitioner& partitioner )
{
  UnitArea    currArea      = partitioner.currArea();

  if( !currArea.Cb().valid() ) return;

  TransformUnit& currTU     = *cs.getTU( currArea.chromaPos(), CH_C );
  const PredictionUnit &pu  = *cs.getPU( currArea.chromaPos(), CH_C );

  CodingStructure &saveCS = *m_pSaveCS[1];
  saveCS.pcv      = cs.pcv;
  saveCS.picture  = cs.picture;
  saveCS.area.repositionTo( cs.area );

  TransformUnit& tmpTU = saveCS.tus.empty() ? saveCS.addTU(currArea, partitioner.chType, nullptr) : *saveCS.tus.front();
  tmpTU.initData();
  tmpTU.UnitArea::operator=( currArea );
  cs.setDecomp(currArea.Cb(), true); // set in advance (required for Cb2/Cr2 in 4:2:2 video)

  const unsigned      numTBlocks  = getNumberValidTBlocks( *cs.pcv );

  CompArea&  cbArea         = currTU.blocks[COMP_Cb];
  CompArea&  crArea         = currTU.blocks[COMP_Cr];
  double     bestCostCb     = MAX_DOUBLE;
  double     bestCostCr     = MAX_DOUBLE;
  Distortion bestDistCb     = 0;
  Distortion bestDistCr     = 0;

  TempCtx ctxStartTU( m_CtxCache );
  TempCtx ctxStart  ( m_CtxCache );
  TempCtx ctxBest   ( m_CtxCache );

  ctxStartTU       = m_CABACEstimator->getCtx();
  currTU.jointCbCr = 0;

  // Do predictions here to avoid repeating the "default0Save1Load2" stuff
  uint32_t  predMode   = PU::getFinalIntraMode( pu, CH_C );

  PelBuf piPredCb = cs.getPredBuf(COMP_Cb);
  PelBuf piPredCr = cs.getPredBuf(COMP_Cr);

  initIntraPatternChType( *currTU.cu, cbArea);
  initIntraPatternChType( *currTU.cu, crArea);

  if( PU::isLMCMode( predMode ) )
  {
    loadLMLumaRecPels( pu, cbArea );
    predIntraChromaLM( COMP_Cb, piPredCb, pu, cbArea, predMode );
    predIntraChromaLM( COMP_Cr, piPredCr, pu, crArea, predMode );
  }
  else
  {
    predIntraAng( COMP_Cb, piPredCb, pu);
    predIntraAng( COMP_Cr, piPredCr, pu);
  }

  // determination of chroma residuals including reshaping and cross-component prediction
  //----- get chroma residuals -----
  PelBuf resiCb  = cs.getResiBuf(COMP_Cb);
  PelBuf resiCr  = cs.getResiBuf(COMP_Cr);
  resiCb.subtract( cs.getOrgBuf (COMP_Cb), piPredCb );
  resiCr.subtract( cs.getOrgBuf (COMP_Cr), piPredCr );

  //----- get reshape parameter ----
  ReshapeData& reshapeData = cs.picture->reshapeData;
  bool doReshaping = ( cs.picHeader->lmcsEnabled && cs.picHeader->lmcsChromaResidualScale && (cs.slice->isIntra() || reshapeData.getCTUFlag()) && (cbArea.width * cbArea.height > 4) );
  if( doReshaping )
  {
    const Area area = currTU.Y().valid() ? currTU.Y() : Area(recalcPosition(currTU.chromaFormat, currTU.chType, CH_L, currTU.blocks[currTU.chType].pos()), recalcSize(currTU.chromaFormat, currTU.chType, CH_L, currTU.blocks[currTU.chType].size()));
    const CompArea& areaY = CompArea(COMP_Y, currTU.chromaFormat, area);
    currTU.chromaAdj = reshapeData.calculateChromaAdjVpduNei(currTU, areaY, currTU.cu->treeType);
  }

  //===== store original residual signals (std and crossCompPred) =====
  CompStorage  orgResiCb[5], orgResiCr[5]; // 0:std, 1-3:jointCbCr (placeholder at this stage), 4:crossComp
  for( int k = 0; k < 1; k+=4 )
  {
    orgResiCb[k].create( cbArea );
    orgResiCr[k].create( crArea );
    orgResiCb[k].copyFrom( resiCb );
    orgResiCr[k].copyFrom( resiCr );

    if( doReshaping )
    {
      int cResScaleInv = currTU.chromaAdj;
      orgResiCb[k].scaleSignal( cResScaleInv, 1, cs.slice->clpRngs[COMP_Cb] );
      orgResiCr[k].scaleSignal( cResScaleInv, 1, cs.slice->clpRngs[COMP_Cr] );
    }
  }

  CUCtx cuCtx;
  cuCtx.isDQPCoded         = true;
  cuCtx.isChromaQpAdjCoded = true;
  cuCtx.lfnstLastScanPos   = false;

  CodingStructure &saveCScur = *m_pSaveCS[2];

  saveCScur.pcv              = cs.pcv;
  saveCScur.picture          = cs.picture;
  saveCScur.area.repositionTo( cs.area );

  TransformUnit& tmpTUcur = saveCScur.tus.empty() ? saveCScur.addTU( currArea, partitioner.chType, nullptr ) : *saveCScur.tus.front();
  tmpTUcur.initData();
  tmpTUcur.UnitArea::operator=( currArea );

  TempCtx ctxBestTUL         ( m_CtxCache );

  const SPS &sps             = *cs.sps;
  double     bestCostCbcur   = MAX_DOUBLE;
  double     bestCostCrcur   = MAX_DOUBLE;
  Distortion bestDistCbcur   = 0;
  Distortion bestDistCrcur   = 0;

  int  endLfnstIdx = ( partitioner.isSepTree( cs ) && partitioner.chType == CH_C && ( partitioner.currArea().lwidth() < 8 || partitioner.currArea().lheight() < 8 ) )
                  || ( partitioner.currArea().lwidth() > sps.getMaxTbSize() || partitioner.currArea().lheight() > sps.getMaxTbSize() ) || !sps.LFNST ? 0 : 2;
  int  startLfnstIdx = 0;
  int  bestLfnstIdx  = 0;
  bool NOTONE_LFNST  = sps.LFNST ? true : false;

  // speedUps LFNST
  bool rapidLFNST = false;
  if (m_pcEncCfg->m_LFNST > 1)
  {
    rapidLFNST = true;
    if (m_pcEncCfg->m_LFNST > 2)
    {
      endLfnstIdx = endLfnstIdx ? 1 : 0;
    }
  }

  if (partitioner.chType != CH_C)
  {
    startLfnstIdx = currTU.cu->lfnstIdx;
    endLfnstIdx   = currTU.cu->lfnstIdx;
    bestLfnstIdx  = currTU.cu->lfnstIdx;
    NOTONE_LFNST  = false;
    rapidLFNST    = false;
  }

  double dSingleCostAll   = MAX_DOUBLE;
  double singleCostTmpAll = 0;

  for (int lfnstIdx = startLfnstIdx; lfnstIdx <= endLfnstIdx; lfnstIdx++)
  {
    if (rapidLFNST && lfnstIdx)
    {
      if ((lfnstIdx == 2) && (bestLfnstIdx == 0))
      {
        continue;
      }
    }

    currTU.cu->lfnstIdx = lfnstIdx;
    if (lfnstIdx)
    {
      m_CABACEstimator->getCtx() = ctxStartTU;
    }

    cuCtx.lfnstLastScanPos = false;
    cuCtx.violatesLfnstConstrained[CH_L]   = false;
    cuCtx.violatesLfnstConstrained[CH_C] = false;

    for( uint32_t c = COMP_Cb; c < numTBlocks; c++)
    {
      const ComponentID compID  = ComponentID(c);
      const CompArea&   area    = currTU.blocks[compID];
      double     dSingleCost    = MAX_DOUBLE;
      Distortion singleDistCTmp = 0;
      double     singleCostTmp  = 0;
      const bool isLastMode     = NOTONE_LFNST || cs.sps->jointCbCr ? false : true;

      if (doReshaping || lfnstIdx)
      {
        resiCb.copyFrom( orgResiCb[0] );
        resiCr.copyFrom( orgResiCr[0] );
      }

      xIntraCodingTUBlock( currTU, compID, false, singleDistCTmp );
      uint64_t fracBitsTmp = xGetIntraFracBitsQTChroma( currTU, compID, &cuCtx );
      singleCostTmp = m_pcRdCost->calcRdCost( fracBitsTmp, singleDistCTmp );

      if( singleCostTmp < dSingleCost )
      {
        dSingleCost = singleCostTmp;

        if ( compID == COMP_Cb )
        {
          bestCostCb = singleCostTmp;
          bestDistCb = singleDistCTmp;
        }
        else
        {
          bestCostCr = singleCostTmp;
          bestDistCr = singleDistCTmp;
        }

        if( !isLastMode )
        {
          saveCS.getRecoBuf(area).copyFrom(cs.getRecoBuf   (area));
          tmpTU.copyComponentFrom(currTU, compID);
          ctxBest = m_CABACEstimator->getCtx();
        }
      }
    }

    singleCostTmpAll = bestCostCb + bestCostCr;

    bool rootCbfL = false;
    if (NOTONE_LFNST)
    {
      for (uint32_t t = 0; t < getNumberValidTBlocks(*cs.pcv); t++)
      {
        rootCbfL |= bool(tmpTU.cbf[t]);
      }
      if (rapidLFNST && !rootCbfL)
      {
        endLfnstIdx = lfnstIdx; // end this
      }
    }

    if (NOTONE_LFNST && lfnstIdx && !cuCtx.lfnstLastScanPos)
    {
      bool cbfAtZeroDepth = currTU.cu->isSepTree()
                              ? rootCbfL : (cs.area.chromaFormat != CHROMA_400
                                 && std::min(tmpTU.blocks[1].width, tmpTU.blocks[1].height) < 4)
                                  ? TU::getCbfAtDepth(currTU, COMP_Y, currTU.depth) : rootCbfL;
      if (cbfAtZeroDepth)
      {
        singleCostTmpAll = MAX_DOUBLE;
      }
    }

    if (NOTONE_LFNST  && (singleCostTmpAll < dSingleCostAll))
    {
      bestLfnstIdx = lfnstIdx;
      if (lfnstIdx != endLfnstIdx)
      {
        dSingleCostAll = singleCostTmpAll;

        bestCostCbcur = bestCostCb;
        bestCostCrcur = bestCostCr;
        bestDistCbcur = bestDistCb;
        bestDistCrcur = bestDistCr;

        saveCScur.getRecoBuf(cbArea).copyFrom(saveCS.getRecoBuf(cbArea));
        saveCScur.getRecoBuf(crArea).copyFrom(saveCS.getRecoBuf(crArea));

        tmpTUcur.copyComponentFrom(tmpTU, COMP_Cb);
        tmpTUcur.copyComponentFrom(tmpTU, COMP_Cr);
      }
      ctxBestTUL = m_CABACEstimator->getCtx();
    }
  }
  if (NOTONE_LFNST && (bestLfnstIdx != endLfnstIdx))
  {
    bestCostCb          = bestCostCbcur;
    bestCostCr          = bestCostCrcur;
    bestDistCb          = bestDistCbcur;
    bestDistCr          = bestDistCrcur;
    currTU.cu->lfnstIdx = bestLfnstIdx;
    if (!cs.sps->jointCbCr)
    {
      cs.getRecoBuf(cbArea).copyFrom(saveCScur.getRecoBuf(cbArea));
      cs.getRecoBuf(crArea).copyFrom(saveCScur.getRecoBuf(crArea));

      currTU.copyComponentFrom(tmpTUcur, COMP_Cb);
      currTU.copyComponentFrom(tmpTUcur, COMP_Cr);

      m_CABACEstimator->getCtx() = ctxBestTUL;
    }
  }

  Distortion bestDistCbCr = bestDistCb + bestDistCr;

  if ( cs.sps->jointCbCr )
  {
    if (NOTONE_LFNST && (bestLfnstIdx != endLfnstIdx))
    {
      saveCS.getRecoBuf(cbArea).copyFrom(saveCScur.getRecoBuf(cbArea));
      saveCS.getRecoBuf(crArea).copyFrom(saveCScur.getRecoBuf(crArea));

      tmpTU.copyComponentFrom(tmpTUcur, COMP_Cb);
      tmpTU.copyComponentFrom(tmpTUcur, COMP_Cr);
      m_CABACEstimator->getCtx() = ctxBestTUL;
      ctxBest                    = m_CABACEstimator->getCtx();
    }
    // Test using joint chroma residual coding
    double     bestCostCbCr   = bestCostCb + bestCostCr;
    int        bestJointCbCr  = 0;
    bool       lastIsBest     = false;
    bool NOLFNST1 = false;
    if (rapidLFNST && (startLfnstIdx != endLfnstIdx))
    {
      if (bestLfnstIdx == 2)
      {
        NOLFNST1 = true;
      }
      else
      {
        endLfnstIdx = 1;
      }
    }

    for (int lfnstIdxj = startLfnstIdx; lfnstIdxj <= endLfnstIdx; lfnstIdxj++)
    {
      if (rapidLFNST && NOLFNST1 && (lfnstIdxj == 1))
      {
        continue;
      }
      currTU.cu->lfnstIdx = lfnstIdxj;
      std::vector<int> jointCbfMasksToTest;
      if (TU::getCbf(tmpTU, COMP_Cb) || TU::getCbf(tmpTU, COMP_Cr))
      {
        jointCbfMasksToTest = m_pcTrQuant->selectICTCandidates(currTU, orgResiCb, orgResiCr);
      }
      for (int cbfMask: jointCbfMasksToTest)
      {
        Distortion distTmp = 0;
        currTU.jointCbCr   = (uint8_t) cbfMask;

        m_CABACEstimator->getCtx() = ctxStartTU;

        resiCb.copyFrom(orgResiCb[cbfMask]);
        resiCr.copyFrom(orgResiCr[cbfMask]);
        cuCtx.lfnstLastScanPos               = false;
        cuCtx.violatesLfnstConstrained[CH_L] = false;
        cuCtx.violatesLfnstConstrained[CH_C] = false;

        xIntraCodingTUBlock(currTU, COMP_Cb, false, distTmp, 0);

        double costTmp = std::numeric_limits<double>::max();
        if (distTmp < MAX_DISTORTION)
        {
          uint64_t bits = xGetIntraFracBitsQTChroma(currTU, COMP_Cb, &cuCtx);
          costTmp       = m_pcRdCost->calcRdCost(bits, distTmp);
        }
        bool rootCbfL = false;
        for (uint32_t t = 0; t < getNumberValidTBlocks(*cs.pcv); t++)
        {
          rootCbfL |= bool(tmpTU.cbf[t]);
        }
        if (rapidLFNST && !rootCbfL)
        {
          endLfnstIdx = lfnstIdxj;
        }
        if (NOTONE_LFNST && currTU.cu->lfnstIdx && !cuCtx.lfnstLastScanPos)
        {
          bool cbfAtZeroDepth = currTU.cu->isSepTree() ? rootCbfL
              : (cs.area.chromaFormat != CHROMA_400 && std::min(tmpTU.blocks[1].width, tmpTU.blocks[1].height) < 4)
              ? TU::getCbfAtDepth(currTU, COMP_Y, currTU.depth) : rootCbfL;
          if (cbfAtZeroDepth)
          {
            costTmp = MAX_DOUBLE;
          }
        }
        if (costTmp < bestCostCbCr)
        {
          bestCostCbCr  = costTmp;
          bestDistCbCr  = distTmp;
          bestJointCbCr = currTU.jointCbCr;

          // store data
          bestLfnstIdx = lfnstIdxj;
          if (cbfMask != jointCbfMasksToTest.back() || (lfnstIdxj != endLfnstIdx))
          {
            saveCS.getRecoBuf(cbArea).copyFrom(cs.getRecoBuf(cbArea));
            saveCS.getRecoBuf(crArea).copyFrom(cs.getRecoBuf(crArea));

            tmpTU.copyComponentFrom(currTU, COMP_Cb);
            tmpTU.copyComponentFrom(currTU, COMP_Cr);

            ctxBest = m_CABACEstimator->getCtx();
          }
          else
          {
            lastIsBest          = true;
            cs.cus[0]->lfnstIdx = bestLfnstIdx;
          }
        }
      }

      // Retrieve the best CU data (unless it was the very last one tested)
    }
    if (!lastIsBest)
    {
      cs.getRecoBuf   (cbArea).copyFrom(saveCS.getRecoBuf   (cbArea));
      cs.getRecoBuf   (crArea).copyFrom(saveCS.getRecoBuf   (crArea));

      cs.cus[0]->lfnstIdx = bestLfnstIdx;
      currTU.copyComponentFrom(tmpTU, COMP_Cb);
      currTU.copyComponentFrom(tmpTU, COMP_Cr);
      m_CABACEstimator->getCtx() = ctxBest;
    }
    currTU.jointCbCr = TU::getCbf(currTU, COMP_Cb) | TU::getCbf(currTU, COMP_Cr) ? bestJointCbCr : 0;
  } // jointCbCr

  cs.dist += bestDistCbCr;
  cuCtx.violatesLfnstConstrained[CH_L] = false;
  cuCtx.violatesLfnstConstrained[CH_C] = false;
  cuCtx.lfnstLastScanPos               = false;
  cuCtx.violatesMtsCoeffConstraint     = false;
  cuCtx.mtsLastScanPos                 = false;
}

uint64_t IntraSearch::xFracModeBitsIntraLuma(const PredictionUnit &pu)
{
  m_CABACEstimator->resetBits();

  if (!pu.ciip)
  {
    m_CABACEstimator->intra_luma_pred_mode(pu);
  }

  return m_CABACEstimator->getEstFracBits();
}

template<typename T, size_t N, int M>
void IntraSearch::xReduceHadCandList(static_vector<T, N>& candModeList, static_vector<double, N>& candCostList, SortedPelUnitBufs<M>& sortedPelBuffer, int& numModesForFullRD, const double thresholdHadCost, const double* mipHadCost, const PredictionUnit &pu, const bool fastMip)
{
  const int maxCandPerType = numModesForFullRD >> 1;
  static_vector<ModeInfo, FAST_UDI_MAX_RDMODE_NUM> tempRdModeList;
  static_vector<double, FAST_UDI_MAX_RDMODE_NUM> tempCandCostList;
  const double minCost = candCostList[0];
  bool keepOneMip = candModeList.size() > numModesForFullRD;
  const int maxNumConv = 3; 

  int numConv = 0;
  int numMip = 0;
  for (int idx = 0; idx < candModeList.size() - (keepOneMip?0:1); idx++)
  {
    bool addMode = false;
    const ModeInfo& orgMode = candModeList[idx];

    if (!orgMode.mipFlg)
    {
      addMode = (numConv < maxNumConv);
      numConv += addMode ? 1:0;
    }
    else
    {
      addMode = ( numMip < maxCandPerType || (candCostList[idx] < thresholdHadCost * minCost) || keepOneMip );
      keepOneMip = false;
      numMip += addMode ? 1:0;
    }
    if( addMode )
    {
      tempRdModeList.push_back(orgMode);
      tempCandCostList.push_back(candCostList[idx]);
    }
  }

  // sort Pel Buffer
  int i = -1;
  for( auto &m: tempRdModeList)
  {
    if( ! (m == candModeList.at( ++i )) )
    {
      for( int j = i; j < (int)candModeList.size()-1; )
      {
        if( m == candModeList.at( ++j ) )
        {
          sortedPelBuffer.swap( i, j);
          break;
        }
      }
    }
  }
  sortedPelBuffer.reduceTo( (int)tempRdModeList.size() );

  if ((pu.lwidth() > 8 && pu.lheight() > 8))
  {
    // Sort MIP candidates by Hadamard cost
    const int transpOff = getNumModesMip(pu.Y());
    static_vector<uint8_t, FAST_UDI_MAX_RDMODE_NUM> sortedMipModes(0);
    static_vector<double, FAST_UDI_MAX_RDMODE_NUM> sortedMipCost(0);
    for (uint8_t mode : { 0, 1, 2 })
    {
      uint8_t candMode = mode + uint8_t((mipHadCost[mode + transpOff] < mipHadCost[mode]) ? transpOff : 0);
      updateCandList(candMode, mipHadCost[candMode], sortedMipModes, sortedMipCost, 3);
    }

    // Append MIP mode to RD mode list
    const int modeListSize = int(tempRdModeList.size());
    for (int idx = 0; idx < 3; idx++)
    {
      const bool     isTransposed = (sortedMipModes[idx] >= transpOff ? true : false);
      const uint32_t mipIdx       = (isTransposed ? sortedMipModes[idx] - transpOff : sortedMipModes[idx]);
      const ModeInfo mipMode( true, isTransposed, 0, NOT_INTRA_SUBPARTITIONS, mipIdx );
      bool alreadyIncluded = false;
      for (int modeListIdx = 0; modeListIdx < modeListSize; modeListIdx++)
      {
        if (tempRdModeList[modeListIdx] == mipMode)
        {
          alreadyIncluded = true;
          break;
        }
      }

      if (!alreadyIncluded)
      {
        tempRdModeList.push_back(mipMode);
        tempCandCostList.push_back(0);
        if( fastMip ) break;
      }
    }
  }

  candModeList = tempRdModeList;
  candCostList = tempCandCostList;
  numModesForFullRD = int(candModeList.size());
}

void IntraSearch::xPreCheckMTS(TransformUnit &tu, std::vector<TrMode> *trModes, const int maxCand, PelUnitBuf *predBuf)
{
  CodingStructure&   cs          = *tu.cs;
  const CompArea&    area        = tu.blocks[COMP_Y];
  const ReshapeData& reshapeData = cs.picture->reshapeData;

  PelBuf piPred    = cs.getPredBuf(COMP_Y);
  PelBuf piResi    = cs.getResiBuf(COMP_Y);

  const PredictionUnit &pu = *cs.getPU(area.pos(), CH_L);
  initIntraPatternChType(*tu.cu, area);
  if( predBuf )
  {
    piPred.copyFrom( predBuf->Y() );
  }
  else if (PU::isMIP(pu, CH_L))
  {
    initIntraMip(pu);
    predIntraMip( piPred, pu);
  }
  else
  {
    predIntraAng( COMP_Y, piPred, pu);
  }

  //===== get residual signal =====
  if (cs.picHeader->lmcsEnabled && reshapeData.getCTUFlag())
  {
    piResi.subtract(cs.getRspOrgBuf(), piPred);
  }
  else
  {
    CPelBuf piOrg = cs.getOrgBuf(COMP_Y);
    piResi.subtract( piOrg, piPred);
  }

  m_pcTrQuant->checktransformsNxN(tu, trModes, m_pcEncCfg->m_MTSIntraMaxCand);
}

} // namespace vvenc

//! \}

