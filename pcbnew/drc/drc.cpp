/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2019 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2014 Dick Hollenbeck, dick@softplc.com
 * Copyright (C) 2017-2020 KiCad Developers, see change_log.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <fctsys.h>
#include <pcb_edit_frame.h>
#include <trigo.h>
#include <board_design_settings.h>
#include <class_edge_mod.h>
#include <class_drawsegment.h>
#include <class_module.h>
#include <class_track.h>
#include <class_pad.h>
#include <class_zone.h>
#include <class_pcb_text.h>
#include <geometry/seg.h>
#include <math_for_graphics.h>
#include <connectivity/connectivity_algo.h>
#include <bitmaps.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_tool_base.h>
#include <tools/zone_filler_tool.h>
#include <kiface_i.h>
#include <pcbnew.h>
#include <drc/drc.h>
#include <netlist_reader/pcb_netlist.h>
#include <math/util.h>      // for KiROUND

#include <dialog_drc.h>
#include <wx/progdlg.h>
#include <board_commit.h>
#include <geometry/shape_arc.h>
#include <drc/drc_rule_parser.h>
#include <drc/drc_item.h>
#include <drc/drc_courtyard_tester.h>
#include <drc/drc_drilled_hole_tester.h>
#include <drc/drc_keepout_tester.h>
#include <drc/drc_netclass_tester.h>
#include <drc/drc_textvar_tester.h>
#include <confirm.h>

DRC::DRC() :
        PCB_TOOL_BASE( "pcbnew.DRCTool" ),
        m_pcbEditorFrame( nullptr ),
        m_pcb( nullptr ),
        m_board_outline_valid( false ),
        m_drcDialog( nullptr ),
        m_largestClearance( 0 )
{
    // establish initial values for everything:
    m_doUnconnectedTest = true;         // enable unconnected tests
    m_doZonesTest = false;              // disable zone to items clearance tests
    m_doKeepoutTest = true;             // enable keepout areas to items clearance tests
    m_refillZones = false;              // Only fill zones if requested by user.
    m_reportAllTrackErrors = false;
    m_testFootprints = false;

    m_drcRun = false;
    m_footprintsTested = false;
}


DRC::~DRC()
{
    for( DRC_ITEM* unconnectedItem : m_unconnected )
        delete unconnectedItem;

    for( DRC_ITEM* footprintItem : m_footprints )
        delete footprintItem;
}


void DRC::Reset( RESET_REASON aReason )
{
    m_pcbEditorFrame = getEditFrame<PCB_EDIT_FRAME>();

    if( m_pcb != m_pcbEditorFrame->GetBoard() )
    {
        if( m_drcDialog )
            DestroyDRCDialog( wxID_OK );

        m_pcb = m_pcbEditorFrame->GetBoard();
    }
}


void DRC::ShowDRCDialog( wxWindow* aParent )
{
    bool show_dlg_modal = true;

    // the dialog needs a parent frame. if it is not specified, this is
    // the PCB editor frame specified in DRC class.
    if( !aParent )
    {
        // if any parent is specified, the dialog is modal.
        // if this is the default PCB editor frame, it is not modal
        show_dlg_modal = false;
        aParent = m_pcbEditorFrame;
    }

    Activate();
    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    if( !m_drcDialog )
    {
        m_drcDialog = new DIALOG_DRC( this, m_pcbEditorFrame, aParent );
        updatePointers();

        if( show_dlg_modal )
            m_drcDialog->ShowModal();
        else
            m_drcDialog->Show( true );
    }
    else    // The dialog is just not visible (because the user has double clicked on an error item)
    {
        updatePointers();
        m_drcDialog->Show( true );
    }
}


int DRC::ShowDRCDialog( const TOOL_EVENT& aEvent )
{
    ShowDRCDialog( nullptr );
    return 0;
}


bool DRC::IsDRCDialogShown()
{
    if( m_drcDialog )
        return m_drcDialog->IsShown();

    return false;
}


void DRC::addMarkerToPcb( BOARD_COMMIT& aCommit, MARKER_PCB* aMarker )
{
    if( m_pcb->GetDesignSettings().Ignore( aMarker->GetRCItem()->GetErrorCode() ) )
    {
        delete aMarker;
        return;
    }

    aCommit.Add( aMarker );
}


void DRC::DestroyDRCDialog( int aReason )
{
    if( m_drcDialog )
    {
        m_drcDialog->Destroy();
        m_drcDialog = nullptr;
    }
}


int DRC::testZoneToZoneOutlines( BOARD_COMMIT& aCommit )
{
    BOARD*   board = m_pcbEditorFrame->GetBoard();
    int      nerrors = 0;

    std::vector<SHAPE_POLY_SET> smoothed_polys;
    smoothed_polys.resize( board->GetAreaCount() );

    for( int ia = 0; ia < board->GetAreaCount(); ia++ )
    {
        ZONE_CONTAINER*    zoneRef = board->GetArea( ia );
        std::set<VECTOR2I> colinearCorners;
        zoneRef->GetColinearCorners( board, colinearCorners );

        zoneRef->BuildSmoothedPoly( smoothed_polys[ia], &colinearCorners );
    }

    // iterate through all areas
    for( int ia = 0; ia < board->GetAreaCount(); ia++ )
    {
        ZONE_CONTAINER* zoneRef = board->GetArea( ia );

        if( !zoneRef->IsOnCopperLayer() )
            continue;

        // If we are testing a single zone, then iterate through all other zones
        // Otherwise, we have already tested the zone combination
        for( int ia2 = ia + 1; ia2 < board->GetAreaCount(); ia2++ )
        {
            ZONE_CONTAINER* zoneToTest = board->GetArea( ia2 );

            if( zoneRef == zoneToTest )
                continue;

            // test for same layer
            if( zoneRef->GetLayer() != zoneToTest->GetLayer() )
                continue;

            // Test for same net
            if( zoneRef->GetNetCode() == zoneToTest->GetNetCode() && zoneRef->GetNetCode() >= 0 )
                continue;

            // test for different priorities
            if( zoneRef->GetPriority() != zoneToTest->GetPriority() )
                continue;

            // test for different types
            if( zoneRef->GetIsKeepout() != zoneToTest->GetIsKeepout() )
                continue;

            // Examine a candidate zone: compare zoneToTest to zoneRef

            // Get clearance used in zone to zone test.  The policy used to
            // obtain that value is now part of the zone object itself by way of
            // ZONE_CONTAINER::GetClearance().
            int zone2zoneClearance = zoneRef->GetClearance( zoneToTest, &m_clearanceSource );

            // Keepout areas have no clearance, so set zone2zoneClearance to 1
            // ( zone2zoneClearance = 0  can create problems in test functions)
            if( zoneRef->GetIsKeepout() )
                zone2zoneClearance = 1;

            // test for some corners of zoneRef inside zoneToTest
            for( auto iterator = smoothed_polys[ia].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia2].Contains( currentVertex ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneRef, zoneToTest );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pt );
                    addMarkerToPcb( aCommit, marker );
                    nerrors++;
                }
            }

            // test for some corners of zoneToTest inside zoneRef
            for( auto iterator = smoothed_polys[ia2].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia].Contains( currentVertex ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneToTest, zoneRef );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pt );
                    addMarkerToPcb( aCommit, marker );
                    nerrors++;
                }
            }

            // Iterate through all the segments of refSmoothedPoly
            std::map<wxPoint, int> conflictPoints;

            for( auto refIt = smoothed_polys[ia].IterateSegmentsWithHoles(); refIt; refIt++ )
            {
                // Build ref segment
                SEG refSegment = *refIt;

                // Iterate through all the segments in smoothed_polys[ia2]
                for( auto testIt = smoothed_polys[ia2].IterateSegmentsWithHoles(); testIt; testIt++ )
                {
                    // Build test segment
                    SEG testSegment = *testIt;
                    wxPoint pt;

                    int ax1, ay1, ax2, ay2;
                    ax1 = refSegment.A.x;
                    ay1 = refSegment.A.y;
                    ax2 = refSegment.B.x;
                    ay2 = refSegment.B.y;

                    int bx1, by1, bx2, by2;
                    bx1 = testSegment.A.x;
                    by1 = testSegment.A.y;
                    bx2 = testSegment.B.x;
                    by2 = testSegment.B.y;

                    int d = GetClearanceBetweenSegments( bx1, by1, bx2, by2,
                                                         0,
                                                         ax1, ay1, ax2, ay2,
                                                         0,
                                                         zone2zoneClearance,
                                                         &pt.x, &pt.y );

                    if( d < zone2zoneClearance )
                    {
                        if( conflictPoints.count( pt ) )
                            conflictPoints[ pt ] = std::min( conflictPoints[ pt ], d );
                        else
                            conflictPoints[ pt ] = d;
                    }
                }
            }

            for( const std::pair<const wxPoint, int>& conflict : conflictPoints )
            {
                int       actual = conflict.second;
                DRC_ITEM* drcItem;

                if( actual <= 0 )
                {
                    drcItem = new DRC_ITEM( DRCE_ZONES_INTERSECT );
                }
                else
                {
                    drcItem = new DRC_ITEM( DRCE_ZONES_TOO_CLOSE );

                    m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                  m_clearanceSource,
                                  MessageTextFromValue( userUnits(), zone2zoneClearance, true ),
                                  MessageTextFromValue( userUnits(), conflict.second, true ) );

                    drcItem->SetErrorMessage( m_msg );
                }

                drcItem->SetItems( zoneRef, zoneToTest );

                MARKER_PCB* marker = new MARKER_PCB( drcItem, conflict.first );
                addMarkerToPcb( aCommit, marker );
                nerrors++;
            }
        }
    }

    return nerrors;
}


void DRC::LoadRules()
{
    wxString   rulesFilepath = m_pcbEditorFrame->Prj().AbsolutePath( "drc-rules" );
    wxFileName rulesFile( rulesFilepath );

    if( rulesFile.FileExists() )
    {
        m_ruleSelectors.clear();
        m_rules.clear();

        FILE* fp = wxFopen( rulesFilepath, wxT( "rt" ) );

        if( fp )
        {
            try
            {
                DRC_RULES_PARSER parser( m_pcb, fp, rulesFilepath );
                parser.Parse( m_ruleSelectors, m_rules );
            }
            catch( PARSE_ERROR& pe )
            {
                // Don't leave possibly malformed stuff around for us to trip over
                m_ruleSelectors.clear();
                m_rules.clear();

                DisplayError( m_drcDialog, pe.What() );
            }
        }
    }

    BOARD_DESIGN_SETTINGS& bds = m_pcb->GetDesignSettings();
    bds.m_DRCRuleSelectors = m_ruleSelectors;
    bds.m_DRCRules = m_rules;
}


void DRC::RunTests( wxTextCtrl* aMessages )
{
    // Make absolutely sure these are up-to-date
    LoadRules();

    wxASSERT( m_pcb == m_pcbEditorFrame->GetBoard() );

    BOARD_COMMIT           commit( m_pcbEditorFrame );
    BOARD_DESIGN_SETTINGS& bds = m_pcb->GetDesignSettings();

    m_largestClearance = bds.GetBiggestClearanceValue();

    if( !bds.Ignore( DRCE_INVALID_OUTLINE )
        || !bds.Ignore( DRCE_TRACK_NEAR_EDGE )
        || !bds.Ignore( DRCE_VIA_NEAR_EDGE )
        || !bds.Ignore( DRCE_PAD_NEAR_EDGE ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Board Outline...\n" ) );
            wxSafeYield();
        }

        testOutline( commit );
    }

    if( aMessages )
    {
        aMessages->AppendText( _( "Netclasses...\n" ) );
        wxSafeYield();
    }

    DRC_NETCLASS_TESTER netclassTester( [&]( MARKER_PCB* aMarker )
                                        {
                                            addMarkerToPcb( commit, aMarker );
                                        } );

    if( !netclassTester.RunDRC( userUnits(), *m_pcb ) )
    {
        // testing the netclasses is a special case because if the netclasses
        // do not pass the BOARD_DESIGN_SETTINGS checks, then every member of a net
        // class (a NET) will cause its items such as tracks, vias, and pads
        // to also fail.  So quit after *all* netclass errors have been reported.
        if( aMessages )
            aMessages->AppendText( _( "NETCLASS VIOLATIONS: Aborting DRC\n" ) );

        commit.Push( wxEmptyString, false, false );

        // update the m_drcDialog listboxes
        updatePointers();

        return;
    }

    // test pad to pad clearances, nothing to do with tracks, vias or zones.
    if( !bds.Ignore( DRCE_PAD_NEAR_EDGE )
        || !bds.Ignore( DRCE_PAD_NEAR_PAD )
        || !bds.Ignore( DRCE_HOLE_NEAR_PAD ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Pad clearances...\n" ) );
            wxSafeYield();
        }

        testPadClearances( commit );
    }

    // test drilled holes
    if( !bds.Ignore( DRCE_DRILLED_HOLES_TOO_CLOSE )
        || !bds.Ignore( DRCE_TOO_SMALL_PAD_DRILL )
        || !bds.Ignore( DRCE_TOO_SMALL_VIA_DRILL )
        || !bds.Ignore( DRCE_TOO_SMALL_MICROVIA_DRILL ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Drill sizes and clearances...\n" ) );
            wxSafeYield();
        }

        DRC_DRILLED_HOLE_TESTER tester( [&]( MARKER_PCB* aMarker )
                                        {
                                            addMarkerToPcb( commit, aMarker );
                                        } );

        tester.RunDRC( userUnits(), *m_pcb );
    }

    // caller (a wxTopLevelFrame) is the wxDialog or the Pcb Editor frame that call DRC:
    wxWindow* caller = aMessages ? aMessages->GetParent() : m_pcbEditorFrame;

    if( m_refillZones )
    {
        if( aMessages )
            aMessages->AppendText( _( "Refilling all zones...\n" ) );

        m_toolMgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( caller );
    }
    else
    {
        if( aMessages )
            aMessages->AppendText( _( "Checking zone fills...\n" ) );

        m_toolMgr->GetTool<ZONE_FILLER_TOOL>()->CheckAllZones( caller );
    }

    // test track and via clearances to other tracks, pads, and vias
    if( aMessages )
    {
        aMessages->AppendText( _( "Track clearances...\n" ) );
        wxSafeYield();
    }

    testTracks( commit, aMessages ? aMessages->GetParent() : m_pcbEditorFrame, true );

    // test zone clearances to other zones
    if( aMessages )
    {
        aMessages->AppendText( _( "Zone to zone clearances...\n" ) );
        wxSafeYield();
    }

    testZones( commit );

    // find and gather unconnected pads.
    if( m_doUnconnectedTest
        && !bds.Ignore( DRCE_UNCONNECTED_ITEMS ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Unconnected pads...\n" ) );
            aMessages->Refresh();
        }

        testUnconnected();
    }

    // find and gather vias, tracks, pads inside keepout areas.
    if( m_doKeepoutTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Keepout areas ...\n" ) );
            aMessages->Refresh();
        }

        DRC_KEEPOUT_TESTER tester( [&]( MARKER_PCB* aMarker )
                                   {
                                       addMarkerToPcb( commit, aMarker );
                                   } );

        tester.RunDRC( userUnits(), *m_pcb );
    }

    // find and gather vias, tracks, pads inside text boxes.
    if( !bds.Ignore( DRCE_VIA_NEAR_COPPER )
        || !bds.Ignore( DRCE_TRACK_NEAR_COPPER ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Text and graphic clearances...\n" ) );
            wxSafeYield();
        }

        testCopperTextAndGraphics( commit );
    }

    // test courtyards
    if( !bds.Ignore( DRCE_OVERLAPPING_FOOTPRINTS )
        || !bds.Ignore( DRCE_MISSING_COURTYARD )
        || !bds.Ignore( DRCE_MALFORMED_COURTYARD )
        || !bds.Ignore( DRCE_PTH_IN_COURTYARD )
        || !bds.Ignore( DRCE_NPTH_IN_COURTYARD ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Courtyard areas...\n" ) );
            aMessages->Refresh();
        }

        DRC_COURTYARD_TESTER tester( [&]( MARKER_PCB* aMarker )
                                     {
                                         addMarkerToPcb( commit, aMarker );
                                     } );

        tester.RunDRC( userUnits(), *m_pcb );
    }

    for( DRC_ITEM* footprintItem : m_footprints )
        delete footprintItem;

    m_footprints.clear();
    m_footprintsTested = false;

    if( m_testFootprints && !Kiface().IsSingle() )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Checking footprints against schematic...\n" ) );
            aMessages->Refresh();
        }

        NETLIST netlist;
        m_pcbEditorFrame->FetchNetlistFromSchematic( netlist, PCB_EDIT_FRAME::ANNOTATION_DIALOG );

        if( m_drcDialog )
            m_drcDialog->Raise();

        TestFootprints( netlist, m_pcb, m_drcDialog->GetUserUnits(), m_footprints );
        m_footprintsTested = true;
    }

    // Check if there are items on disabled layers
    if( !bds.Ignore( DRCE_DISABLED_LAYER_ITEM ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Items on disabled layers...\n" ) );
            aMessages->Refresh();
        }

        testDisabledLayers( commit );
    }

    if( !bds.Ignore( DRCE_UNRESOLVED_VARIABLE ) )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Unresolved text variables...\n" ) );
            aMessages->Refresh();
        }

        DRC_TEXTVAR_TESTER tester( [&]( MARKER_PCB* aMarker )
                                   {
                                       addMarkerToPcb( commit, aMarker );
                                   } );

        tester.RunDRC( userUnits(), *m_pcb );
    }

    commit.Push( wxEmptyString, false, false );
    m_drcRun = true;

    // update the m_drcDialog listboxes
    updatePointers();

    if( aMessages )
    {
        // no newline on this one because it is last, don't want the window
        // to unnecessarily scroll.
        aMessages->AppendText( _( "Finished" ) );
    }
}


void DRC::updatePointers()
{
    // update my pointers, m_pcbEditorFrame is the only unchangeable one
    m_pcb = m_pcbEditorFrame->GetBoard();

    m_pcbEditorFrame->ResolveDRCExclusions();

    if( m_drcDialog )  // Use diag list boxes only in DRC dialog
    {
        m_drcDialog->SetMarkersProvider( new BOARD_DRC_ITEMS_PROVIDER( m_pcb ) );
        m_drcDialog->SetUnconnectedProvider( new RATSNEST_DRC_ITEMS_PROVIDER( m_pcbEditorFrame,
                                                                              &m_unconnected ) );
        m_drcDialog->SetFootprintsProvider( new VECTOR_DRC_ITEMS_PROVIDER( m_pcbEditorFrame,
                                                                           &m_footprints ) );
    }
}


void DRC::testPadClearances( BOARD_COMMIT& aCommit )
{
    BOARD_DESIGN_SETTINGS& bds = m_pcb->GetDesignSettings();
    std::vector<D_PAD*>    sortedPads;

    m_pcb->GetSortedPadListByXthenYCoord( sortedPads );

    if( sortedPads.empty() )
        return;

    // find the max size of the pads (used to stop the pad-to-pad tests)
    int max_size = 0;

    for( D_PAD* pad : sortedPads )
    {
        // GetBoundingRadius() is the radius of the minimum sized circle fully containing the pad
        int radius = pad->GetBoundingRadius();

        if( radius > max_size )
            max_size = radius;
    }

    // Better to be fast than accurate; this keeps us from having to look up / calculate the
    // actual clearances
    max_size += m_largestClearance;

    // Upper limit of pad list (limit not included)
    D_PAD** listEnd = &sortedPads[0] + sortedPads.size();

    // Test the pads
    for( auto& pad : sortedPads )
    {
        if( !bds.Ignore( DRCE_PAD_NEAR_EDGE ) && m_board_outline_valid )
        {
            static DRAWSEGMENT dummyEdge;
            dummyEdge.SetLayer( Edge_Cuts );

            int minClearance = pad->GetClearance( &dummyEdge, &m_clearanceSource );

            if( bds.m_CopperEdgeClearance > minClearance )
            {
                minClearance = bds.m_CopperEdgeClearance;
                m_clearanceSource = _( "board edge" );
            }

            for( auto it = m_board_outlines.IterateSegmentsWithHoles(); it; it++ )
            {
                int actual;

                if( !checkClearanceSegmToPad( *it, 0, pad, minClearance, &actual ) )
                {
                    actual = std::max( 0, actual );
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_EDGE );

                    m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                  m_clearanceSource,
                                  MessageTextFromValue( userUnits(), minClearance, true ),
                                  MessageTextFromValue( userUnits(), actual, true ) );

                    drcItem->SetErrorMessage( m_msg );
                    drcItem->SetItems( pad );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
                    addMarkerToPcb( aCommit, marker );

                    break;
                }
            }
        }

        if( !bds.Ignore( DRCE_PAD_NEAR_PAD ) || !bds.Ignore( DRCE_HOLE_NEAR_PAD ) )
        {
            int x_limit = pad->GetPosition().x + pad->GetBoundingRadius() + max_size;

            doPadToPadsDrc( aCommit, pad, &pad, listEnd, x_limit );
        }
    }
}


void DRC::testTracks( BOARD_COMMIT& aCommit, wxWindow *aActiveWindow, bool aShowProgressBar )
{
    wxProgressDialog* progressDialog = NULL;
    const int         delta = 500;  // This is the number of tests between 2 calls to the
                                    // progress bar
    int               count = m_pcb->Tracks().size();
    int               deltamax = count/delta;

    if( aShowProgressBar && deltamax > 3 )
    {
        // Do not use wxPD_APP_MODAL style here: it is not necessary and create issues
        // on OSX
        progressDialog = new wxProgressDialog( _( "Track clearances" ), wxEmptyString,
                                               deltamax, aActiveWindow,
                                               wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME );
        progressDialog->Update( 0, wxEmptyString );
    }

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = m_pcb->GetConnectivity();
    BOARD_DESIGN_SETTINGS&             settings = m_pcb->GetDesignSettings();


    if( !m_pcb->GetDesignSettings().Ignore( DRCE_DANGLING_TRACK )
            || !m_pcb->GetDesignSettings().Ignore( DRCE_DANGLING_VIA ) )
    {
        connectivity->Clear();
        connectivity->Build( m_pcb ); // just in case. This really needs to be reliable.
    }

    int ii = 0;
    count = 0;

    for( auto seg_it = m_pcb->Tracks().begin(); seg_it != m_pcb->Tracks().end(); seg_it++ )
    {
        if( ii++ > delta )
        {
            ii = 0;
            count++;

            if( progressDialog )
            {
                if( !progressDialog->Update( count, wxEmptyString ) )
                    break;  // Aborted by user
#ifdef __WXMAC__
                // Work around a dialog z-order issue on OS X
                if( count == deltamax )
                    aActiveWindow->Raise();
#endif
            }
        }

        // Test new segment against tracks and pads, optionally against copper zones
        doTrackDrc( aCommit, *seg_it, seg_it + 1, m_pcb->Tracks().end(), m_doZonesTest );

        // Test for dangling items
        int code = (*seg_it)->Type() == PCB_VIA_T ? DRCE_DANGLING_VIA : DRCE_DANGLING_TRACK;
        wxPoint pos;

        if( !settings.Ignore( code ) && connectivity->TestTrackEndpointDangling( *seg_it, &pos ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( code );
            drcItem->SetItems( *seg_it );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, pos );
            addMarkerToPcb( aCommit, marker );
        }
    }

    if( progressDialog )
        progressDialog->Destroy();
}


void DRC::testUnconnected()
{
    for( DRC_ITEM* unconnectedItem : m_unconnected )
        delete unconnectedItem;

    m_unconnected.clear();

    auto connectivity = m_pcb->GetConnectivity();

    connectivity->Clear();
    connectivity->Build( m_pcb ); // just in case. This really needs to be reliable.
    connectivity->RecalculateRatsnest();

    std::vector<CN_EDGE> edges;
    connectivity->GetUnconnectedEdges( edges );

    for( const CN_EDGE& edge : edges )
    {
        DRC_ITEM* item = new DRC_ITEM( DRCE_UNCONNECTED_ITEMS );
        item->SetItems( edge.GetSourceNode()->Parent(), edge.GetTargetNode()->Parent() );
        m_unconnected.push_back( item );
    }
}


void DRC::testZones( BOARD_COMMIT& aCommit )
{
    // Test copper areas for valid netcodes
    // if a netcode is < 0 the netname was not found when reading a netlist
    // if a netcode is == 0 the netname is void, and the zone is not connected.
    // This is allowed, but i am not sure this is a good idea
    //
    // In recent Pcbnew versions, the netcode is always >= 0, but an internal net name
    // is stored, and initialized from the file or the zone properties editor.
    // if it differs from the net name from net code, there is a DRC issue
    if( !m_pcb->GetDesignSettings().Ignore( DRCE_ZONE_HAS_EMPTY_NET ) )
    {
        for( int ii = 0; ii < m_pcb->GetAreaCount(); ii++ )
        {
            ZONE_CONTAINER* zone = m_pcb->GetArea( ii );

            if( !zone->IsOnCopperLayer() )
                continue;

            int netcode = zone->GetNetCode();
            // a netcode < 0 or > 0 and no pad in net  is a error or strange
            // perhaps a "dead" net, which happens when all pads in this net were removed
            // Remark: a netcode < 0 should not happen (this is more a bug somewhere)
            int pads_in_net = ( netcode > 0 ) ? m_pcb->GetConnectivity()->GetPadCount( netcode ) : 1;

            if( ( netcode < 0 ) || pads_in_net == 0 )
            {
                DRC_ITEM* drcItem = new DRC_ITEM( DRCE_ZONE_HAS_EMPTY_NET );
                drcItem->SetItems( zone );

                MARKER_PCB* marker = new MARKER_PCB( drcItem, zone->GetPosition() );
                addMarkerToPcb( aCommit, marker );
            }
        }
    }

    // Test copper areas outlines, and create markers when needed
    testZoneToZoneOutlines( aCommit );
}


void DRC::testCopperTextAndGraphics( BOARD_COMMIT& aCommit )
{
    // Test copper items for clearance violations with vias, tracks and pads

    for( BOARD_ITEM* brdItem : m_pcb->Drawings() )
    {
        if( IsCopperLayer( brdItem->GetLayer() ) )
            testCopperDrawItem( aCommit, brdItem );
    }

    for( MODULE* module : m_pcb->Modules() )
    {
        TEXTE_MODULE& ref = module->Reference();
        TEXTE_MODULE& val = module->Value();

        if( ref.IsVisible() && IsCopperLayer( ref.GetLayer() ) )
            testCopperDrawItem( aCommit, &ref );

        if( val.IsVisible() && IsCopperLayer( val.GetLayer() ) )
            testCopperDrawItem( aCommit, &val );

        if( module->IsNetTie() )
            continue;

        for( BOARD_ITEM* item : module->GraphicalItems() )
        {
            if( IsCopperLayer( item->GetLayer() ) )
            {
                if( item->Type() == PCB_MODULE_TEXT_T && ( (TEXTE_MODULE*) item )->IsVisible() )
                    testCopperDrawItem( aCommit, item );
                else if( item->Type() == PCB_MODULE_EDGE_T )
                    testCopperDrawItem( aCommit, item );
            }
        }
    }
}


void DRC::testCopperDrawItem( BOARD_COMMIT& aCommit, BOARD_ITEM* aItem )
{
    EDA_RECT         bbox;
    std::vector<SEG> itemShape;
    int              itemWidth;
    DRAWSEGMENT*     drawItem = dynamic_cast<DRAWSEGMENT*>( aItem );
    EDA_TEXT*        textItem = dynamic_cast<EDA_TEXT*>( aItem );

    if( drawItem )
    {
        bbox = drawItem->GetBoundingBox();
        itemWidth = drawItem->GetWidth();

        switch( drawItem->GetShape() )
        {
        case S_ARC:
        {
            SHAPE_ARC arc( drawItem->GetCenter(), drawItem->GetArcStart(),
                           (double) drawItem->GetAngle() / 10.0 );

            SHAPE_LINE_CHAIN l = arc.ConvertToPolyline();

            for( int i = 0; i < l.SegmentCount(); i++ )
                itemShape.push_back( l.Segment( i ) );

            break;
        }

        case S_SEGMENT:
            itemShape.emplace_back( SEG( drawItem->GetStart(), drawItem->GetEnd() ) );
            break;

        case S_CIRCLE:
        {
            // SHAPE_CIRCLE has no ConvertToPolyline() method, so use a 360.0 SHAPE_ARC
            SHAPE_ARC circle( drawItem->GetCenter(), drawItem->GetEnd(), 360.0 );

            SHAPE_LINE_CHAIN l = circle.ConvertToPolyline();

            for( int i = 0; i < l.SegmentCount(); i++ )
                itemShape.push_back( l.Segment( i ) );

            break;
        }

        case S_CURVE:
        {
            drawItem->RebuildBezierToSegmentsPointsList( drawItem->GetWidth() );
            wxPoint start_pt = drawItem->GetBezierPoints()[0];

            for( unsigned int jj = 1; jj < drawItem->GetBezierPoints().size(); jj++ )
            {
                wxPoint end_pt = drawItem->GetBezierPoints()[jj];
                itemShape.emplace_back( SEG( start_pt, end_pt ) );
                start_pt = end_pt;
            }

            break;
        }

        default:
            break;
        }
    }
    else if( textItem  )
    {
        bbox = textItem->GetTextBox();
        itemWidth = textItem->GetEffectiveTextPenWidth();

        std::vector<wxPoint> textShape;
        textItem->TransformTextShapeToSegmentList( textShape );

        for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
            itemShape.emplace_back( SEG( textShape[jj], textShape[jj+1] ) );
    }
    else
    {
        wxFAIL_MSG( "unknown item type in testCopperDrawItem()" );
        return;
    }

    SHAPE_RECT rect_area( bbox.GetX(), bbox.GetY(), bbox.GetWidth(), bbox.GetHeight() );

    // Test tracks and vias
    for( auto track : m_pcb->Tracks() )
    {
        if( !track->IsOnLayer( aItem->GetLayer() ) )
            continue;

        int minClearance = track->GetClearance( aItem, &m_clearanceSource );
        int widths = ( track->GetWidth() + itemWidth ) / 2;
        int center2centerAllowed = minClearance + widths;

        SEG trackSeg( track->GetStart(), track->GetEnd() );

        // Fast test to detect a track segment candidate inside the text bounding box
        if( !rect_area.Collide( trackSeg, center2centerAllowed ) )
            continue;

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( const SEG& itemSeg : itemShape )
        {
            SEG::ecoord thisDist_squared = trackSeg.SquaredDistance( itemSeg );

            if( !minSeg || thisDist_squared < center2center_squared )
            {
                minSeg = itemSeg;
                center2center_squared = thisDist_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            int       errorCode = ( track->Type() == PCB_VIA_T ) ? DRCE_VIA_NEAR_COPPER
                                                                 : DRCE_TRACK_NEAR_COPPER;
            DRC_ITEM* drcItem = new DRC_ITEM( errorCode );

            m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                          m_clearanceSource,
                          MessageTextFromValue( userUnits(), minClearance, true ),
                          MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( track, aItem );

            wxPoint     pos = GetLocation( track, minSeg.get() );
            MARKER_PCB* marker = new MARKER_PCB( drcItem, pos );
            addMarkerToPcb( aCommit, marker );
        }
    }

    // Test pads
    for( auto pad : m_pcb->GetPads() )
    {
        if( !pad->IsOnLayer( aItem->GetLayer() ) )
            continue;

        // Graphic items are allowed to act as net-ties within their own footprint
        if( drawItem && pad->GetParent() == drawItem->GetParent() )
            continue;

        int minClearance = pad->GetClearance( aItem, &m_clearanceSource );
        int widths = itemWidth / 2;
        int center2centerAllowed = minClearance + widths;

        // Fast test to detect a pad candidate inside the text bounding box
        // Finer test (time consumming) is made only for pads near the text.
        int      bb_radius = pad->GetBoundingRadius() + minClearance;
        VECTOR2I shape_pos( pad->ShapePos() );

        if( !rect_area.Collide( SEG( shape_pos, shape_pos ), bb_radius ) )
            continue;

        SHAPE_POLY_SET padOutline;
        pad->TransformShapeWithClearanceToPolygon( padOutline, 0 );

        OPT<SEG>    minSeg;
        SEG::ecoord center2center_squared = 0;

        for( const SEG& itemSeg : itemShape )
        {
            SEG::ecoord thisCenter2center_squared = padOutline.SquaredDistance( itemSeg );

            if( !minSeg || thisCenter2center_squared < center2center_squared )
            {
                minSeg = itemSeg;
                center2center_squared = thisCenter2center_squared;
            }
        }

        if( center2center_squared < SEG::Square( center2centerAllowed ) )
        {
            int       actual = std::max( 0.0, sqrt( center2center_squared ) - widths );
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_COPPER );

            m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                          m_clearanceSource,
                          MessageTextFromValue( userUnits(), minClearance, true ),
                          MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( pad, aItem );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
            addMarkerToPcb( aCommit, marker );
        }
    }
}


void DRC::testOutline( BOARD_COMMIT& aCommit )
{
    wxPoint error_loc( m_pcb->GetBoardEdgesBoundingBox().GetPosition() );

    m_board_outlines.RemoveAllContours();
    m_board_outline_valid = false;

    if( m_pcb->GetBoardPolygonOutlines( m_board_outlines, nullptr, &error_loc ) )
    {
        m_board_outline_valid = true;
    }
    else
    {
        DRC_ITEM* drcItem = new DRC_ITEM( DRCE_INVALID_OUTLINE );

        m_msg.Printf( drcItem->GetErrorText() + _( " (not a closed shape)" ) );

        drcItem->SetErrorMessage( m_msg );
        drcItem->SetItems( m_pcb );

        MARKER_PCB* marker = new MARKER_PCB( drcItem, error_loc );
        addMarkerToPcb( aCommit, marker );
    }
}


void DRC::testDisabledLayers( BOARD_COMMIT& aCommit )
{
    BOARD*   board = m_pcbEditorFrame->GetBoard();
    wxCHECK( board, /*void*/ );

    LSET     disabledLayers = board->GetEnabledLayers().flip();

    // Perform the test only for copper layers
    disabledLayers &= LSET::AllCuMask();

    for( TRACK* track : board->Tracks() )
    {
        if( disabledLayers.test( track->GetLayer() ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

            m_msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                          track->GetLayerName() );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( track );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, track->GetPosition() );
            addMarkerToPcb( aCommit, marker );
        }
    }

    for( MODULE* module : board->Modules() )
    {
        module->RunOnChildren(
                    [&]( BOARD_ITEM* child )
                    {
                        if( disabledLayers.test( child->GetLayer() ) )
                        {
                            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

                            m_msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                                          child->GetLayerName() );

                            drcItem->SetErrorMessage( m_msg );
                            drcItem->SetItems( child );

                            MARKER_PCB* marker = new MARKER_PCB( drcItem, child->GetPosition() );
                            addMarkerToPcb( aCommit, marker );
                        }
                    } );
    }

    for( ZONE_CONTAINER* zone : board->Zones() )
    {
        if( disabledLayers.test( zone->GetLayer() ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_DISABLED_LAYER_ITEM );

            m_msg.Printf( drcItem->GetErrorText() + _( "layer %s" ),
                          zone->GetLayerName() );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( zone );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, zone->GetPosition() );
            addMarkerToPcb( aCommit, marker );
        }
    }
}


bool DRC::doPadToPadsDrc( BOARD_COMMIT& aCommit, D_PAD* aRefPad, D_PAD** aStart, D_PAD** aEnd,
                          int x_limit )
{
    const static LSET all_cu = LSET::AllCuMask();

    LSET layerMask = aRefPad->GetLayerSet() & all_cu;

    /* used to test DRC pad to holes: this dummy pad has the size and shape of the hole
     * to test pad to pad hole DRC, using the pad to pad DRC test function.
     * Therefore, this dummy pad is a circle or an oval.
     * A pad must have a parent because some functions expect a non null parent
     * to find the parent board, and some other data
     */
    MODULE dummymodule( m_pcb );    // Creates a dummy parent
    D_PAD  dummypad( &dummymodule );

    // Ensure the hole is on all copper layers
    dummypad.SetLayerSet( all_cu | dummypad.GetLayerSet() );

    for( D_PAD** pad_list = aStart;  pad_list<aEnd;  ++pad_list )
    {
        D_PAD* pad = *pad_list;

        if( pad == aRefPad )
            continue;

        // We can stop the test when pad->GetPosition().x > x_limit
        // because the list is sorted by X values
        if( pad->GetPosition().x > x_limit )
            break;

        // No problem if pads which are on copper layers are on different copper layers,
        // (pads can be only on a technical layer, to build complex pads)
        // but their hole (if any ) can create DRC error because they are on all
        // copper layers, so we test them
        if( ( pad->GetLayerSet() & layerMask ) == 0 &&
            ( pad->GetLayerSet() & all_cu ) != 0 &&
            ( aRefPad->GetLayerSet() & all_cu ) != 0 )
        {
            // if holes are in the same location and have the same size and shape,
            // this can be accepted
            if( pad->GetPosition() == aRefPad->GetPosition()
                && pad->GetDrillSize() == aRefPad->GetDrillSize()
                && pad->GetDrillShape() == aRefPad->GetDrillShape() )
            {
                if( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_CIRCLE )
                    continue;

                // for oval holes: must also have the same orientation
                if( pad->GetOrientation() == aRefPad->GetOrientation() )
                    continue;
            }

            /* Here, we must test clearance between holes and pads
             * dummy pad size and shape is adjusted to pad drill size and shape
             */
            if( pad->GetDrillSize().x )
            {
                // pad under testing has a hole, test this hole against pad reference
                dummypad.SetPosition( pad->GetPosition() );
                dummypad.SetSize( pad->GetDrillSize() );
                dummypad.SetShape( pad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                           PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( pad->GetOrientation() );

                int minClearance = aRefPad->GetClearance( nullptr, &m_clearanceSource );
                int actual;

                if( !checkClearancePadToPad( aRefPad, &dummypad, minClearance, &actual ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_HOLE_NEAR_PAD );

                    m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                  m_clearanceSource,
                                  MessageTextFromValue( userUnits(), minClearance, true ),
                                  MessageTextFromValue( userUnits(), actual, true ) );

                    drcItem->SetErrorMessage( m_msg );
                    drcItem->SetItems( pad, aRefPad );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, pad->GetPosition() );
                    addMarkerToPcb( aCommit, marker );
                    return false;
                }
            }

            if( aRefPad->GetDrillSize().x ) // pad reference has a hole
            {
                dummypad.SetPosition( aRefPad->GetPosition() );
                dummypad.SetSize( aRefPad->GetDrillSize() );
                dummypad.SetShape( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                               PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( aRefPad->GetOrientation() );

                int minClearance = pad->GetClearance( nullptr, &m_clearanceSource );
                int actual;

                if( !checkClearancePadToPad( pad, &dummypad, minClearance, &actual ) )
                {
                    DRC_ITEM* drcItem = new DRC_ITEM( DRCE_HOLE_NEAR_PAD );

                    m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                                  m_clearanceSource,
                                  MessageTextFromValue( userUnits(), minClearance, true ),
                                  MessageTextFromValue( userUnits(), actual, true ) );

                    drcItem->SetErrorMessage( m_msg );
                    drcItem->SetItems( aRefPad, pad );

                    MARKER_PCB* marker = new MARKER_PCB( drcItem, aRefPad->GetPosition() );
                    addMarkerToPcb( aCommit, marker );
                    return false;
                }
            }

            continue;
        }

        // The pad must be in a net (i.e pt_pad->GetNet() != 0 ),
        // But no problem if pads have the same netcode (same net)
        if( pad->GetNetCode() && ( aRefPad->GetNetCode() == pad->GetNetCode() ) )
            continue;

        // if pads are from the same footprint
        if( pad->GetParent() == aRefPad->GetParent() )
        {
            // and have the same pad number ( equivalent pads  )

            // one can argue that this 2nd test is not necessary, that any
            // two pads from a single module are acceptable.  This 2nd test
            // should eventually be a configuration option.
            if( pad->PadNameEqual( aRefPad ) )
                continue;
        }

        // if either pad has no drill and is only on technical layers, not a clearance violation
        if( ( ( pad->GetLayerSet() & layerMask ) == 0 && !pad->GetDrillSize().x ) ||
            ( ( aRefPad->GetLayerSet() & layerMask ) == 0 && !aRefPad->GetDrillSize().x ) )
        {
            continue;
        }

        int minClearance = aRefPad->GetClearance( nullptr, &m_clearanceSource );
        int actual;

        if( !checkClearancePadToPad( aRefPad, pad, minClearance, &actual ) )
        {
            DRC_ITEM* drcItem = new DRC_ITEM( DRCE_PAD_NEAR_PAD );

            m_msg.Printf( drcItem->GetErrorText() + _( " (%s %s; actual %s)" ),
                          m_clearanceSource,
                          MessageTextFromValue( userUnits(), minClearance, true ),
                          MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( aRefPad, pad );

            MARKER_PCB* marker = new MARKER_PCB( drcItem, aRefPad->GetPosition() );
            addMarkerToPcb( aCommit, marker );
            return false;
        }
    }

    return true;
}


void DRC::TestFootprints( NETLIST& aNetlist, BOARD* aPCB, EDA_UNITS aUnits,
                          std::vector<DRC_ITEM*>& aDRCList )
{
    wxString msg;

    auto comp = []( const MODULE* x, const MODULE* y )
    {
        return x->GetReference().CmpNoCase( y->GetReference() ) < 0;
    };
    auto mods = std::set<MODULE*, decltype( comp )>( comp );

    if( !aPCB->GetDesignSettings().Ignore( DRCE_DUPLICATE_FOOTPRINT ) )
    {
        // Search for duplicate footprints on the board
        for( MODULE* mod : aPCB->Modules() )
        {
            auto ins = mods.insert( mod );

            if( !ins.second )
            {
                DRC_ITEM* item = new DRC_ITEM( DRCE_DUPLICATE_FOOTPRINT );
                item->SetItems( mod, *ins.first );
                aDRCList.push_back( item );
            }
        }
    }

    if( !aPCB->GetDesignSettings().Ignore( DRCE_MISSING_FOOTPRINT ) )
    {
        // Search for component footprints in the netlist but not on the board.
        for( unsigned ii = 0; ii < aNetlist.GetCount(); ii++ )
        {
            COMPONENT* component = aNetlist.GetComponent( ii );
            MODULE*    module = aPCB->FindModuleByReference( component->GetReference() );

            if( module == NULL )
            {
                msg.Printf( _( "Missing footprint %s (%s)" ),
                            component->GetReference(),
                            component->GetValue() );

                DRC_ITEM* item = new DRC_ITEM( DRCE_MISSING_FOOTPRINT );
                item->SetErrorMessage( msg );
                aDRCList.push_back( item );
            }
        }
    }

    if( !aPCB->GetDesignSettings().Ignore( DRCE_EXTRA_FOOTPRINT ) )
    {
        // Search for component footprints found on board but not in netlist.
        for( auto module : mods )
        {
            COMPONENT* component = aNetlist.GetComponentByReference( module->GetReference() );

            if( component == NULL )
            {
                DRC_ITEM* item = new DRC_ITEM( DRCE_EXTRA_FOOTPRINT );
                item->SetItems( module );
                aDRCList.push_back( item );
            }
        }
    }
}


void DRC::setTransitions()
{
    Go( &DRC::ShowDRCDialog,              PCB_ACTIONS::runDRC.MakeEvent() );
}


const int EPSILON = Mils2iu( 5 );


wxPoint DRC::GetLocation( TRACK* aTrack, ZONE_CONTAINER* aConflictZone )
{
    SHAPE_POLY_SET* conflictOutline;

    if( aConflictZone->IsFilled() )
        conflictOutline = const_cast<SHAPE_POLY_SET*>( &aConflictZone->GetFilledPolysList() );
    else
        conflictOutline = aConflictZone->Outline();

    wxPoint pt1 = aTrack->GetPosition();
    wxPoint pt2 = aTrack->GetEnd();

    // If the mid-point is in the zone, then that's a fine place for the marker
    if( conflictOutline->SquaredDistance( ( pt1 + pt2 ) / 2 ) == 0 )
        return ( pt1 + pt2 ) / 2;

    // Otherwise do a binary search for a "good enough" marker location
    else
    {
        while( GetLineLength( pt1, pt2 ) > EPSILON )
        {
            if( conflictOutline->SquaredDistance( pt1 ) < conflictOutline->SquaredDistance( pt2 ) )
                pt2 = ( pt1 + pt2 ) / 2;
            else
                pt1 = ( pt1 + pt2 ) / 2;
        }

        // Once we're within EPSILON pt1 and pt2 are "equivalent"
        return pt1;
    }
}


wxPoint DRC::GetLocation( TRACK* aTrack, const SEG& aConflictSeg )
{
    wxPoint pt1 = aTrack->GetPosition();
    wxPoint pt2 = aTrack->GetEnd();

    // Do a binary search along the track for a "good enough" marker location
    while( GetLineLength( pt1, pt2 ) > EPSILON )
    {
        if( aConflictSeg.SquaredDistance( pt1 ) < aConflictSeg.SquaredDistance( pt2 ) )
            pt2 = ( pt1 + pt2 ) / 2;
        else
            pt1 = ( pt1 + pt2 ) / 2;
    }

    // Once we're within EPSILON pt1 and pt2 are "equivalent"
    return pt1;
}


