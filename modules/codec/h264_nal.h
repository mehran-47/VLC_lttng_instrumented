/*****************************************************************************
 * Copyright © 2010-2011 VideoLAN
 *
 * Authors: Jean-Baptiste Kempf <jb@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* Parse the SPS/PPS Metadata and convert it to annex b format */
static int convert_sps_pps( decoder_t *p_dec, const uint8_t *p_buf,
                            uint32_t i_buf_size, uint8_t *p_out_buf,
                            uint32_t i_out_buf_size, uint32_t *p_sps_pps_size,
                            uint32_t *p_nal_size)
{
    int i_profile;
    uint32_t i_data_size = i_buf_size, i_nal_size, i_sps_pps_size = 0;
    unsigned int i_loop_end;

    /* */
    if( i_data_size < 7 )
    {
        msg_Err( p_dec, "Input Metadata too small" );
        return VLC_ENOMEM;
    }

    /* Read infos in first 6 bytes */
    i_profile    = (p_buf[1] << 16) | (p_buf[2] << 8) | p_buf[3];
    if (p_nal_size)
        *p_nal_size  = (p_buf[4] & 0x03) + 1;
    p_buf       += 5;
    i_data_size -= 5;

    for ( unsigned int j = 0; j < 2; j++ )
    {
        /* First time is SPS, Second is PPS */
        if( i_data_size < 1 )
        {
            msg_Err( p_dec, "PPS too small after processing SPS/PPS %u",
                    i_data_size );
            return VLC_ENOMEM;
        }
        i_loop_end = p_buf[0] & (j == 0 ? 0x1f : 0xff);
        p_buf++; i_data_size--;

        for ( unsigned int i = 0; i < i_loop_end; i++)
        {
            if( i_data_size < 2 )
            {
                msg_Err( p_dec, "SPS is too small %u", i_data_size );
                return VLC_ENOMEM;
            }

            i_nal_size = (p_buf[0] << 8) | p_buf[1];
            p_buf += 2;
            i_data_size -= 2;

            if( i_data_size < i_nal_size )
            {
                msg_Err( p_dec, "SPS size does not match NAL specified size %u",
                        i_data_size );
                return VLC_ENOMEM;
            }
            if( i_sps_pps_size + 4 + i_nal_size > i_out_buf_size )
            {
                msg_Err( p_dec, "Output SPS/PPS buffer too small" );
                return VLC_ENOMEM;
            }

            p_out_buf[i_sps_pps_size++] = 0;
            p_out_buf[i_sps_pps_size++] = 0;
            p_out_buf[i_sps_pps_size++] = 0;
            p_out_buf[i_sps_pps_size++] = 1;

            memcpy( p_out_buf + i_sps_pps_size, p_buf, i_nal_size );
            i_sps_pps_size += i_nal_size;

            p_buf += i_nal_size;
            i_data_size -= i_nal_size;
        }
    }

    *p_sps_pps_size = i_sps_pps_size;

    return VLC_SUCCESS;
}

