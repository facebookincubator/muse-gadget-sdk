/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
namespace muse_eink {
// Optional board renderer; callbacks run on the UI task after begin().
struct BoardRendering {
    void (*begin)(void *display);
    void (*present)(bool clean);
    void (*invalidate)();
    bool (*pending)();
};
const BoardRendering *board_rendering();
}
