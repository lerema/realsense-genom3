/*
 * Copyright (c) 2019 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution  and  use  in  source  and binary  forms,  with  or  without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 * THE SOFTWARE  IS PROVIDED "AS IS"  AND THE AUTHOR  DISCLAIMS ALL WARRANTIES
 * WITH  REGARD   TO  THIS  SOFTWARE  INCLUDING  ALL   IMPLIED  WARRANTIES  OF
 * MERCHANTABILITY AND  FITNESS.  IN NO EVENT  SHALL THE AUTHOR  BE LIABLE FOR
 * ANY  SPECIAL, DIRECT,  INDIRECT, OR  CONSEQUENTIAL DAMAGES  OR  ANY DAMAGES
 * WHATSOEVER  RESULTING FROM  LOSS OF  USE, DATA  OR PROFITS,  WHETHER  IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR  OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *                                             Martin Jacquet - September 2022
 *
 * Modified by: Selvakumar H S - August 2023
 */

#include "acrealsense.h"

#include "realsense_c_types.h"

#include "codels.hh"

#include <err.h>
#include <chrono>
#include <cmath>

/* --- Task visual ------------------------------------------------------ */

/** Codel rs_viz_start of task visual.
 *
 * Triggered by realsense_start.
 * Yields to realsense_sleep.
 */
genom_event
rs_viz_start(const realsense_frame *frame, realsense_undist_s **undist,
             or_camera_info *info, const genom_context self)
{
    *undist = new realsense_undist_s();
    info->compression_rate = -1;
    info->frequency = 30;
    snprintf(info->format, sizeof(info->format), "RGB8");
    info->size = {1280, 720};

    frame->open("FE_l", self);
    frame->open("FE_l/jpeg", self);
    frame->open("FE_r", self);
    frame->open("FE_r/jpeg", self);

    (void)genom_sequence_reserve(&(frame->data("FE_l", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("FE_l/jpeg", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("FE_r", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("FE_r/jpeg", self)->pixels), 0);

    frame->open("color", self);
    frame->open("color/jpeg", self);
    frame->open("IR_l", self);
    frame->open("IR_l/jpeg", self);
    frame->open("IR_r", self);
    frame->open("IR_r/jpeg", self);

    (void)genom_sequence_reserve(&(frame->data("color", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("color/jpeg", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("IR_l", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("IR_l/jpeg", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("IR_r", self)->pixels), 0);
    (void)genom_sequence_reserve(&(frame->data("IR_r/jpeg", self)->pixels), 0);

    return realsense_sleep;
}

/** Codel rs_viz_sleep of task visual.
 *
 * Triggered by realsense_sleep.
 * Yields to realsense_pause_sleep, realsense_poll.
 */
genom_event
rs_viz_sleep(bool started, const genom_context self)
{
    if (!started)
        return realsense_pause_sleep;
    else
        return realsense_poll;
}

/** Codel rs_viz_poll of task visual.
 *
 * Triggered by realsense_poll.
 * Yields to realsense_sleep, realsense_main.
 */
genom_event
rs_viz_poll(realsense_sync_s **v_sync, or_camera_data **v_data,
            const genom_context self)
{
    std::unique_lock<std::mutex> lock((*v_sync)->_sync->m);

    if (!(*v_sync)->_sync->frames->size())
    {
        (*v_sync)->_sync->cv.wait_for(lock, std::chrono::duration<int16_t>(realsense_poll_duration_sec));

        if (!(*v_sync)->_sync->frames->size())
            return realsense_sleep;
    }

    rs2::frame f;
    while ((*v_sync)->_sync->frames->size())
    {
        (*v_sync)->_sync->frames->poll_for_frame(&f);
        (*v_data)->_data.enqueue(f);
    }

    lock.unlock();

    return realsense_main;
}

/** Codel rs_viz_main of task visual.
 *
 * Triggered by realsense_main.
 * Yields to realsense_sleep.
 */
genom_event
rs_viz_main(int16_t compression_rate, const or_camera_data *v_data,
            const realsense_undist_s *undist,
            const realsense_frame *frame, const genom_context self)
{
    rs2::frame f;
    uint16_t i = 0; // counter to discriminate IR and FE frames
    while (v_data->_data.size())
    {
        v_data->_data.poll_for_frame(&f);
        rs2::video_frame fv = f.as<rs2::video_frame>();

        // Get corresponding output port
        rs2_stream type = fv.get_profile().stream_type();
        std::string port_name;
        if (type == RS2_STREAM_FISHEYE && i == 0)
            port_name = "FE_l";
        if (type == RS2_STREAM_FISHEYE && i == 1)
            port_name = "FE_r";
        if (type == RS2_STREAM_COLOR)
            port_name = "color";
        if (type == RS2_STREAM_INFRARED && i == 0)
            port_name = "IR_l";
        if (type == RS2_STREAM_INFRARED && i == 1)
            port_name = "IR_r";

        const void *pixel_data;
        uint16_t w, h, bpp;
        // Undistort if requested
        if (undist->enabled && type == RS2_STREAM_FISHEYE)
        {
            Mat cvframe = Mat(
                Size(fv.get_width(), fv.get_height()),
                CV_8UC1,
                (void *)f.get_data(),
                Mat::AUTO_STEP);

            remap(cvframe, cvframe, undist->m1, undist->m2, INTER_LINEAR);

            pixel_data = cvframe.data;
            w = cvframe.size().height;
            h = cvframe.size().width;
            bpp = cvframe.elemSize();
        }
        else
        {
            pixel_data = f.get_data();
            w = fv.get_width();
            h = fv.get_height();
            bpp = fv.get_bytes_per_pixel();
        }

        // Update port data lenght and reallocate if need be
        or_sensor_frame *port_raw_data = frame->data(port_name.c_str(), self);
        if (h * w * bpp != port_raw_data->pixels._length)
        {
            if (h * w * bpp > port_raw_data->pixels._maximum && genom_sequence_reserve(&(port_raw_data->pixels), h * w * bpp) == -1)
            {
                realsense_e_mem_detail d;
                snprintf(d.what, sizeof(d.what), "unable to allocate frame memory");
                warnx("%s", d.what);
                return realsense_e_mem(&d, self);
            }
            port_raw_data->pixels._length = h * w * bpp;
            port_raw_data->height = h;
            port_raw_data->width = w;
            port_raw_data->bpp = bpp;
            port_raw_data->compressed = false;
        }

        // Copy data on port, update timestamp and write
        memcpy(port_raw_data->pixels._buffer, pixel_data, port_raw_data->pixels._length);
        double ms = fv.get_timestamp();
        port_raw_data->ts.sec = floor(ms / 1000);
        port_raw_data->ts.nsec = (ms - (double)port_raw_data->ts.sec * 1000) * 1e6;

        frame->write(port_name.c_str(), self);

        // compress image if requested
        if (compression_rate != -1)
        {
            std::vector<int32_t> compression_params;
            compression_params.push_back(IMWRITE_JPEG_QUALITY);
            compression_params.push_back(compression_rate);

            Mat cvframe = Mat(
                Size(w, h),
                CV_8UC1,
                port_raw_data->pixels._buffer,
                Mat::AUTO_STEP);
            std::vector<uint8_t> buf;
            imencode(".jpg", cvframe, buf, compression_params);

            // Update port data lenght and reallocate if need be
            or_sensor_frame *port_jpeg_data = frame->data((port_name + "/jpeg").c_str(), self);
            if (buf.size() != port_jpeg_data->pixels._length)
            {
                if (buf.size() > port_jpeg_data->pixels._maximum &&
                    genom_sequence_reserve(&(port_jpeg_data->pixels), buf.size()) == -1)
                {
                    realsense_e_mem_detail d;
                    snprintf(d.what, sizeof(d.what), "unable to allocate frame memory");
                    warnx("%s", d.what);
                    return realsense_e_mem(&d, self);
                }
                port_jpeg_data->pixels._length = buf.size();
                port_jpeg_data->height = h;
                port_jpeg_data->width = w;
                port_jpeg_data->bpp = bpp;
                port_jpeg_data->compressed = true;
            }

            // Copy data on port, update timestamp and write
            memcpy(port_jpeg_data->pixels._buffer, buf.data(), buf.size());
            port_jpeg_data->ts = port_raw_data->ts;

            frame->write((port_name + "/jpeg").c_str(), self);
        }

        i++;
    }

    return realsense_sleep;
}

/* --- Activity set_undistortion ---------------------------------------- */

/** Codel rs_set_undistort of activity set_undistortion.
 *
 * Triggered by realsense_start.
 * Yields to realsense_ether.
 * Throws realsense_e_io.
 */
genom_event
rs_set_undistort(uint16_t size, float fov, const or_camera_pipe *pipe,
                 realsense_undist_s **undist,
                 const realsense_intrinsics *intrinsics,
                 const genom_context self)
{
    // check if either COLOR or FISHEYE is enabled, cannot compute undistortion map without initial calibration
    rs2_intrinsics *intr = &pipe->cam->_intr;
    if (intr->width == 0)
    {
        realsense_e_io_detail d;
        snprintf(d.what, sizeof(d.what), "fisheye stream not enabled, cannot compute undistortion map");
        warnx("%s", d.what);
        return realsense_e_io(&d, self);
    }

    or_sensor_intrinsics *intr_data = intrinsics->data(self);

    if (size == 0)
    {
        *intr_data = {
            .calib = {
                intr->fx,
                intr->fy,
                intr->ppx,
                intr->ppy,
                0,
            },
            .disto = {
                intr->coeffs[0],
                intr->coeffs[1],
                intr->coeffs[2],
                intr->coeffs[3],
                intr->coeffs[4],
            },
        };
        intrinsics->write(self);

        (*undist)->enabled = false;
        warnx("stop undistortion");
    }
    else
    {
        // Get current calibration
        Mat K = Mat::zeros(3, 3, CV_32F);
        K.at<float>(0, 0) = intr->fx;
        K.at<float>(1, 1) = intr->fy;
        K.at<float>(0, 2) = intr->ppx;
        K.at<float>(1, 2) = intr->ppy;
        K.at<float>(0, 1) = 0;
        K.at<float>(2, 2) = 1;
        Mat D = (Mat_<float>(4, 1) << intr->coeffs[0],
                 intr->coeffs[1],
                 intr->coeffs[2],
                 intr->coeffs[3]);

        // Compute desired calibration
        float f_px = size / 2 / tan(fov / 2);
        float c = size / 2;
        Mat P = (Mat_<float>(3, 3) << f_px, 0, c,
                 0, f_px, c,
                 0, 0, 1);

        // Compute undistortion maps
        fisheye::initUndistortRectifyMap(K, D, Mat::eye(3, 3, CV_32F), P, Size(size, size), CV_16SC2, (*undist)->m1, (*undist)->m2);

        // Publish intrinsincs with 'fake distortion' (=0) since the image is undistorted before publishing
        *intr_data = {
            .calib = {
                f_px,
                f_px,
                c,
                c,
                0,
            },
            .disto = {
                0,
                0,
                0,
                0,
                0,
            },
        };
        intrinsics->write(self);

        (*undist)->enabled = true;

        warnx("new undistortion maps computed");
    }

    return realsense_ether;
}
