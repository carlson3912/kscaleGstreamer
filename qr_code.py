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

def create_display():
    """Create the main display with background image and QR code overlay"""
    # Get screen dimensions
    root = tk.Tk()
    screen_width = root.winfo_screenwidth()
    screen_height = root.winfo_screenheight()
    
    # Load and process the background image
    try:
        bg_image = Image.open("images/hello.webp")
        
        # Calculate scaling to fit the image optimally while maintaining aspect ratio
        img_width, img_height = bg_image.size
        scale_w = screen_width / img_width
        scale_h = screen_height / img_height
        scale = min(scale_w, scale_h)  # Use the smaller scale to ensure it fits
        
        # Resize the image
        new_width = int(img_width * scale)
        new_height = int(img_height * scale)
        bg_image = bg_image.resize((new_width, new_height), Image.Resampling.LANCZOS)
        
        # Create a black background canvas
        canvas_image = Image.new('RGB', (screen_width, screen_height), 'black')
        
        # Center the background image on the black canvas
        x_offset = (screen_width - new_width) // 2
        y_offset = (screen_height - new_height) // 2
        canvas_image.paste(bg_image, (x_offset, y_offset))
        
    except FileNotFoundError:
        print("Warning: hello.webp not found, using solid black background")
        canvas_image = Image.new('RGB', (screen_width, screen_height), 'black')
    
    # Generate QR code
    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=8,  # Smaller box size for top-right positioning
        border=2,    # Smaller border
    )
    qr.add_data(data)
    qr.make(fit=True)
    
    qr_img = qr.make_image(fill_color="white", back_color="black")
    
    # Position QR code in top right with margins
    qr_size = qr_img.size[0]  # QR codes are square
    margin = 20  # Margin from edges
    qr_x = screen_width - qr_size - margin
    qr_y = margin
    
    # Paste QR code onto the canvas
    canvas_image.paste(qr_img, (qr_x, qr_y))
    
    return root, canvas_image

# Signal handler for graceful shutdown
def signal_handler(sig, frame):
    print("\nShutting down...")
    if 'root' in globals():
        root.quit()
        root.destroy()
    sys.exit(0)

# Register signal handler for Ctrl+C
signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

# Create the display
root, canvas_image = create_display()
root.title("QR Code Display")

# Convert PIL image to Tkinter image
tk_img = ImageTk.PhotoImage(canvas_image)

# Create label with black background and pack
label = tk.Label(root, image=tk_img, bg='black')
label.pack(fill=tk.BOTH, expand=True)

# Configure root window
root.configure(bg='black')  # Set window background to black
root.attributes("-fullscreen", True)
root.bind("<Escape>", lambda e: root.destroy())  # Press Esc to quit

# Handle window close button
root.protocol("WM_DELETE_WINDOW", lambda: signal_handler(None, None))

print(f"Display created with QR code containing: {data}")
print("Press Escape to quit or use Ctrl+C")

try:
    root.mainloop()
except KeyboardInterrupt:
    signal_handler(None, None)
