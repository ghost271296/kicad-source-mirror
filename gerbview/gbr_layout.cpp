/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2012-2018 Jean-Pierre Charras  jp.charras at wanadoo.fr
 * Copyright (C) 1992-2018 KiCad Developers, see AUTHORS.txt for contributors.
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
 * @file gbr_layout.cpp
 * @brief  GBR_LAYOUT class functions.
 */

#include <fctsys.h>
#include <gr_basic.h>
#include <gr_text.h>
#include <gerbview_frame.h>
#include <class_drawpanel.h>
#include <gbr_layout.h>
#include <gerber_file_image.h>
#include <gerber_file_image_list.h>

GBR_LAYOUT::GBR_LAYOUT() :
    EDA_ITEM( (EDA_ITEM*)NULL, GERBER_LAYOUT_T )
{
}


GBR_LAYOUT::~GBR_LAYOUT()
{
}

// Accessor to the list of gerber files (and drill files) images
GERBER_FILE_IMAGE_LIST* GBR_LAYOUT::GetImagesList() const
{
    return &GERBER_FILE_IMAGE_LIST::GetImagesList();
}


EDA_RECT GBR_LAYOUT::ComputeBoundingBox() const
{
    EDA_RECT bbox;
    bool first_item = true;

    for( unsigned layer = 0; layer < GetImagesList()->ImagesMaxCount(); ++layer )
    {
        GERBER_FILE_IMAGE* gerber = GetImagesList()->GetGbrImage( layer );

        if( gerber == NULL )    // Graphic layer not yet used
            continue;

        for( GERBER_DRAW_ITEM* item = gerber->GetItemsList(); item; item = item->Next() )
        {
            if( first_item )
            {
                bbox = item->GetBoundingBox();
                first_item = false;
            }
            else
                bbox.Merge( item->GetBoundingBox() );
        }
    }

    bbox.Normalize();

    m_BoundingBox = bbox;
    return bbox;
}


SEARCH_RESULT GBR_LAYOUT::Visit( INSPECTOR inspector, void* testData, const KICAD_T scanTypes[] )
{
    KICAD_T        stype;
    SEARCH_RESULT  result = SEARCH_CONTINUE;
    const KICAD_T* p    = scanTypes;
    bool           done = false;

#if 0 && defined(DEBUG)
    std::cout << GetClass().mb_str() << ' ';
#endif

    while( !done )
    {
        stype = *p;

        switch( stype )
        {
        case GERBER_IMAGE_LIST_T:
            for( unsigned layer = 0; layer < GetImagesList()->ImagesMaxCount(); ++layer )
            {
                GERBER_FILE_IMAGE* gerber = GetImagesList()->GetGbrImage( layer );

                if( gerber == NULL )    // Graphic layer not yet used
                    continue;

                result = gerber->Visit( inspector, testData, p );

                if( result == SEARCH_QUIT )
                    break;
            }

            ++p;
            break;

        default:        // catch EOT or ANY OTHER type here and return.
            done = true;
            break;
        }

        if( result == SEARCH_QUIT )
            break;
    }

    return result;
}
