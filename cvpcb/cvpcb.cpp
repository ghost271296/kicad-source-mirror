/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2007 Jean-Pierre Charras, jaen-pierre.charras@gipsa-lab.inpg.com
 * Copyright (C) 1992-2012 KiCad Developers, see AUTHORS.txt for contributors.
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

/**
 * @file cvpcb.cpp
 */

#include <fctsys.h>
#include <macros.h>
#include <fp_lib_table.h>
#include <gr_basic.h>
#include <kiface_i.h>
#include <pgm_base.h>
#include <wxstruct.h>
#include <confirm.h>
#include <gestfich.h>

#include <cvpcb.h>
#include <zones.h>
#include <cvpcb_mainframe.h>
#include <colors_selection.h>
#include <cvpcb_id.h>

#include <build_version.h>

#include <wx/snglinst.h>

// Colors for layers and items
COLORS_DESIGN_SETTINGS g_ColorsSettings;

// Constant string definitions for CvPcb
const wxString RetroFileExtension( wxT( "stf" ) );
const wxString FootprintAliasFileExtension( wxT( "equ" ) );

// Wildcard for schematic retroannotation (import footprint names in schematic):
const wxString FootprintAliasFileWildcard( _( "KiCad footprint alias files (*.equ)|*.equ" ) );

const wxString titleLibLoadError( _( "Library Load Error" ) );


#if 0   // add this logic to OpenProjectFiles()

/*
 * MacOSX: Needed for file association
 * http://wiki.wxwidgets.org/WxMac-specific_topics
 */
void PGM_BASE::MacOpenFile( const wxString& aFileName )
{
    wxFileName  filename = aFileName;
    wxString    oldPath;

    CVPCB_MAINFRAME* frame = (CVPCB_MAINFRAME*) GetTopWindow();

    if( !filename.FileExists() )
        return;

    if( frame->m_NetlistFileName.DirExists() )
        oldPath = frame->m_NetlistFileName.GetPath();

    // Update the library search path list.
    if( Pgm().GetLibraryPathList().Index( oldPath ) != wxNOT_FOUND )
        Pgm().GetLibraryPathList().Remove( oldPath );

    Pgm().GetLibraryPathList().Insert( filename.GetPath(), 0 );

    frame->m_NetlistFileName = filename;
    frame->ReadNetListAndLinkFiles();
}
#endif


namespace CV {

static struct IFACE : public KIFACE_I
{
    // Of course all are virtual overloads, implementations of the KIFACE.

    IFACE( const char* aName, KIWAY::FACE_T aType ) :
        KIFACE_I( aName, aType )
    {}

    bool OnKifaceStart( PGM_BASE* aProgram, int aCtlBits );

    void OnKifaceEnd();

    wxWindow* CreateWindow( wxWindow* aParent, int aClassId, KIWAY* aKiway, int aCtlBits = 0 )
    {
        switch( aClassId )
        {
        case CVPCB_FRAME_TYPE:
            {
                CVPCB_MAINFRAME* frame = new CVPCB_MAINFRAME( aKiway, aParent );
                return frame;
            }
            break;

        default:
            ;
        }

        return NULL;
    }

    /**
     * Function IfaceOrAddress
     * return a pointer to the requested object.  The safest way to use this
     * is to retrieve a pointer to a static instance of an interface, similar to
     * how the KIFACE interface is exported.  But if you know what you are doing
     * use it to retrieve anything you want.
     *
     * @param aDataId identifies which object you want the address of.
     *
     * @return void* - and must be cast into the know type.
     */
    void* IfaceOrAddress( int aDataId )
    {
        return NULL;
    }

} kiface( "cvpcb", KIWAY::FACE_CVPCB );

} // namespace

using namespace CV;


static PGM_BASE* process;


KIFACE_I& Kiface() { return kiface; }


// KIFACE_GETTER's actual spelling is a substitution macro found in kiway.h.
// KIFACE_GETTER will not have name mangling due to declaration in kiway.h.
MY_API( KIFACE* ) KIFACE_GETTER(  int* aKIFACEversion, int aKIWAYversion, PGM_BASE* aProgram )
{
    process = (PGM_BASE*) aProgram;
    return &kiface;
}


PGM_BASE& Pgm()
{
    wxASSERT( process );    // KIFACE_GETTER has already been called.
    return *process;
}


/**
 * Function set3DShapesPath
 * attempts to set the environment variable given by aKiSys3Dmod to a valid path.
 * (typically "KISYS3DMOD" )
 * If the environment variable is already set,
 * then it left as is to respect the wishes of the user.
 *
 * The path is determined by attempting to find the path modules/packages3d
 * files in kicad tree.
 * This may or may not be the best path but it provides the best solution for
 * backwards compatibility with the previous 3D shapes search path implementation.
 *
 * @note This must be called after #SetBinDir() is called at least on Windows.
 * Otherwise, the kicad path is not known (Windows specific)
 *
 * @param aKiSys3Dmod = the value of environment variable, typically "KISYS3DMOD"
 * @return false if the aKiSys3Dmod path is not valid.
 */
static bool set3DShapesPath( const wxString& aKiSys3Dmod )
{
    wxString    path;

    // Set the KISYS3DMOD environment variable for the current process,
    // if it is not already defined in the user's environment and valid.
    if( wxGetEnv( aKiSys3Dmod, &path ) && wxFileName::DirExists( path ) )
        return true;

    // Attempt to determine where the 3D shape libraries were installed using the
    // legacy path:
    // on Unix: /usr/local/kicad/share/modules/packages3d
    // or  /usr/share/kicad/modules/packages3d
    // On Windows: bin../share/modules/packages3d
    wxString relpath( wxT( "modules/packages3d" ) );

// Apple MacOSx
#ifdef __WXMAC__
    path = wxT("/Library/Application Support/kicad/modules/packages3d/");

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }

    path = wxString( wxGetenv( wxT( "HOME" ) ) ) + wxT("/Library/Application Support/kicad/modules/packages3d/");

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }

#elif defined(__UNIX__)     // Linux and non-Apple Unix
    // Try the home directory:
    path.Empty();
    wxGetEnv( wxT("HOME"), &path );
    path += wxT("/kicad/share/") + relpath;

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }

    // Try the standard install path:
    path = wxT("/usr/local/kicad/share/") + relpath;

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }

    // Try the official distrib standard install path:
    path = wxT("/usr/share/kicad/") + relpath;

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }

#else   // Windows
    // On Windows, the install path is given by the path of executables
    wxFileName fn;
    fn.AssignDir( Pgm().GetExecutablePath() );
    fn.RemoveLastDir();
    path = fn.GetPathWithSep() + wxT("share/") + relpath;

    if( wxFileName::DirExists( path ) )
    {
        wxSetEnv( aKiSys3Dmod, path );
        return true;
    }
#endif

    return false;
}


//!!!!!!!!!!!!!!! This code is obsolete because of the merge into pcbnew, don't bother with it.

FP_LIB_TABLE GFootprintTable;


// A short lived implementation.  cvpcb will get combine into pcbnew shortly, so
// we skip setting KISYSMOD here for now.  User should set the environment
// variable.

bool IFACE::OnKifaceStart( PGM_BASE* aProgram, int aCtlBits )
{
    // This is process level, not project level, initialization of the DSO.

    // Do nothing in here pertinent to a project!

    start_common( aCtlBits );

    // Set 3D shape path from environment variable KISYS3DMOD
    set3DShapesPath( wxT("KISYS3DMOD") );

    /*  Now that there are no *.mod files in the standard library, this function
        has no utility.  User should simply set the variable manually.
        Looking for *.mod files which do not exist is fruitless.

    // SetFootprintLibTablePath();
    */

    g_DrawBgColor = BLACK;

    try
    {
        // The global table is not related to a specific project.  All projects
        // will use the same global table.  So the KIFACE::OnKifaceStart() contract
        // of avoiding anything project specific is not violated here.

        if( !FP_LIB_TABLE::LoadGlobalTable( GFootprintTable ) )
        {
            DisplayInfoMessage( NULL, wxT(
                "You have run CvPcb for the first time using the "
                "new footprint library table method for finding "
                "footprints.  CvPcb has either copied the default "
                "table or created an empty table in your home "
                "folder.  You must first configure the library "
                "table to include all footprint libraries not "
                "included with KiCad.  See the \"Footprint Library "
                "Table\" section of the CvPcb documentation for "
                "more information." ) );
        }
    }
    catch( const IO_ERROR& ioe )
    {
        wxString msg = wxString::Format( _(
            "An error occurred attempting to load the global footprint library "
            "table:\n\n%s" ),
            GetChars( ioe.errorText )
            );
        DisplayError( NULL, msg );
        return false;
    }

    return true;
}

void IFACE::OnKifaceEnd()
{
    end_common();
}
