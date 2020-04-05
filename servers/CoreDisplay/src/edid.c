/**
 * Copyright (c) 2020 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "stdbool.h"
#include "stdint.h"
#include "CoreDisplay/edid.h"

bool coredisplay_parse_edid(uint8_t *raw, edid_t *result)
{
    if (!(raw[0] == 0 && raw[1] == 0xff && raw[2] == 0xff && raw[3] == 0xff &&
          raw[4] == 0xff && raw[5] == 0xff && raw[6] == 0xff && raw[7] == 0x00))
        return false;

    // Detect port type
    if (raw[20] >> 7)
    { // Only handle digital ports for now
        result->port_type = (raw[20]) & 7;
        switch ((raw[20] >> 4) & 7)
        {
        case 0:
            result->bit_depth = 0;
            break;
        case 1:
            result->bit_depth = 6;
            break;
        case 2:
            result->bit_depth = 8;
            break;
        case 3:
            result->bit_depth = 10;
            break;
        case 4:
            result->bit_depth = 12;
            break;
        case 5:
            result->bit_depth = 14;
            break;
        case 6:
            result->bit_depth = 16;
            break;
        default:
            return false;
        }
    }

    result->gamma = raw[23];
    result->established_modes =
        raw[35] | ((uint32_t)raw[36] << 8) | ((uint32_t)raw[37] << 16);

    result->standard_timing_count = 0;
    for (int i = 0; i < 16; i += 2)
        if (!(raw[38 + i] == 0x01 &&
              raw[39 + i] == 0x01))
        { // ensure this entry is valid
            result->standard_timings[result->standard_timing_count].h_res =
                (raw[38 + i] + 31) * 8;
            result->standard_timings[result->standard_timing_count].v_freq =
                (raw[39 + i] & 0x1F) + 60;
            switch ((raw[39 + i] >> 6))
            {
            case 0:
                result->standard_timings[result->standard_timing_count].aspect_num = 16;
                result->standard_timings[result->standard_timing_count].aspect_denom =
                    10;
                break;
            case 1:
                result->standard_timings[result->standard_timing_count].aspect_num = 4;
                result->standard_timings[result->standard_timing_count].aspect_denom =
                    3;
                break;
            case 2:
                result->standard_timings[result->standard_timing_count].aspect_num = 5;
                result->standard_timings[result->standard_timing_count].aspect_denom =
                    4;
                break;
            case 3:
                result->standard_timings[result->standard_timing_count].aspect_num = 16;
                result->standard_timings[result->standard_timing_count].aspect_denom =
                    9;
                break;
            }

            result->standard_timings[result->standard_timing_count].v_res =
                (result->standard_timings[result->standard_timing_count].h_res *
                 result->standard_timings[result->standard_timing_count]
                     .aspect_denom) /
                result->standard_timings[result->standard_timing_count].aspect_num;

            result->standard_timing_count++;
        }

    result->detailed_mode_count = 0;
    for (int i = 0; i < 4 * 18; i += 18)
    {
        if (raw[54 + i] == 0 && raw[54 + i + 1] == 0)
        {
            // display descriptor
            if (raw[54 + i + 3] == 0xFC)
                for (int n_i = 0; n_i < 13; n_i++)
                    result->display_name[n_i] = raw[54 + i + 5];
        }
        else
        {
            // timing descriptor
            result->detailed_modes[result->detailed_mode_count].pixel_clock =
                ((uint16_t)raw[54 + i + 1] << 8) + raw[54 + i];
            result->detailed_modes[result->detailed_mode_count].hactive =
                raw[54 + i + 2] + ((uint16_t)(raw[54 + i + 4] >> 4) << 8);
            result->detailed_modes[result->detailed_mode_count].hblank =
                raw[54 + i + 3] + ((uint16_t)(raw[54 + i + 4] & 0xF) << 8);

            result->detailed_modes[result->detailed_mode_count].vactive =
                raw[54 + i + 5] + ((uint16_t)(raw[54 + i + 7] >> 4) << 8);
            result->detailed_modes[result->detailed_mode_count].vblank =
                raw[54 + i + 6] + ((uint16_t)(raw[54 + i + 7] & 0xF) << 8);

            result->detailed_modes[result->detailed_mode_count].hsync_porch =
                raw[54 + i + 8] + ((uint16_t)(raw[54 + i + 11] >> 6) << 8);
            result->detailed_modes[result->detailed_mode_count].hsync_pulse =
                raw[54 + i + 9] + ((uint16_t)((raw[54 + i + 11] >> 4) & 0x3) << 8);

            result->detailed_modes[result->detailed_mode_count].vsync_porch =
                (raw[54 + i + 10] >> 4) + ((uint16_t)((raw[54 + i + 11] >> 2) & 0x3)
                                           << 4);
            result->detailed_modes[result->detailed_mode_count].vsync_pulse =
                (raw[54 + i + 10] & 0xf) + ((uint16_t)(raw[54 + i + 11] & 0x3) << 4);

            result->detailed_modes[result->detailed_mode_count].hsize_mm =
                raw[54 + i + 12] + (((uint16_t)raw[54 + i + 14] >> 4) << 8);
            result->detailed_modes[result->detailed_mode_count].vsize_mm =
                raw[54 + i + 13] + (((uint16_t)raw[54 + i + 14] & 0xf) << 8);

            result->detailed_modes[result->detailed_mode_count].hborder =
                raw[54 + i + 15];
            result->detailed_modes[result->detailed_mode_count].vborder =
                raw[54 + i + 16];

            if ((raw[54 + i + 17] >> 4) & 1)
            {
                if ((raw[54 + i + 17] >> 3) & 1)
                {
                    result->detailed_modes[result->detailed_mode_count].v_polarity =
                        (raw[54 + i + 17] >> 2) & 1;
                    result->detailed_modes[result->detailed_mode_count].h_polarity =
                        (raw[54 + i + 17] >> 1) & 1;
                }
                else
                {
                    result->detailed_modes[result->detailed_mode_count].v_polarity =
                        (raw[54 + i + 17] >> 2) & 1;
                    result->detailed_modes[result->detailed_mode_count].h_polarity =
                        (raw[54 + i + 17] >> 2) & 1;
                }
            }
            else
                return false; // don't support analong modes

            result->detailed_mode_count++;
        }
    }

    return true;
}