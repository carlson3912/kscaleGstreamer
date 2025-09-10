import cv2 as cv
import numpy as np

# Camera intrinsics
cam_mat = np.array([
    [297.80062345, 0., 685.72493754],
    [0., 298.63865273, 451.61133244],
    [0., 0., 1.0]
])

dist_coeffs = np.array([[-0.20148179, 0.03270111, 0., 0., -0.00211291]])
DIM = (1280, 720)

new_cam_mat, _ = cv.getOptimalNewCameraMatrix(cam_mat, dist_coeffs, DIM, 1, DIM)
map1, map2 = cv.initUndistortRectifyMap(cam_mat, dist_coeffs, None, new_cam_mat, DIM, cv.CV_16SC2)

# OpenCV filter function for GStreamer
def undistort_gst(frame: np.ndarray) -> np.ndarray:
    return cv.remap(frame, map1, map2, interpolation=cv.INTER_LINEAR, borderMode=cv.BORDER_CONSTANT)
