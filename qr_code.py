# qr_display.py
import qrcode
from PIL import Image, ImageTk
import tkinter as tk
from dotenv import load_dotenv
import os
import signal
import sys

# Replace with your ID and secret
load_dotenv()
user_id = os.getenv('ROBOT_ID')
secret_key = os.getenv('AUTH_TOKEN')
ip_address = os.getenv('MY_IP')

# Combine them into a string (you can use JSON or any format)
data = f"{user_id}:{secret_key}:{ip_address}"

# Generate QR code
qr = qrcode.QRCode(
    version=1,
    error_correction=qrcode.constants.ERROR_CORRECT_H,
    box_size=10,
    border=4,
)
qr.add_data(data)
qr.make(fit=True)

img = qr.make_image(fill_color="black", back_color="white")

# Signal handler for graceful shutdown
def signal_handler(sig, frame):
    print("\nShutting down...")
    root.quit()
    root.destroy()
    sys.exit(0)

# Register signal handler for Ctrl+C
signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

# Display using Tkinter
root = tk.Tk()
root.title("QR Code Display")

# Convert PIL image to Tkinter image
tk_img = ImageTk.PhotoImage(img)

# Create label and pack
label = tk.Label(root, image=tk_img)
label.pack()

# Make window fullscreen (optional)
root.attributes("-fullscreen", True)
root.bind("<Escape>", lambda e: root.destroy())  # Press Esc to quit

# Handle window close button
root.protocol("WM_DELETE_WINDOW", lambda: signal_handler(None, None))

try:
    root.mainloop()
except KeyboardInterrupt:
    signal_handler(None, None)
