import asyncio
import threading
import pygame
from bleak import BleakClient, BleakScanner, BleakError

CHAR_UUID = "ff01"
FULL_CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
CALIBRATION_CHAR_UUID = "ff02"
FULL_CALIBRATION_CHAR_UUID = "0000ff02-0000-1000-8000-00805f9b34fb"

DEVICE_NAME = "BLE_Encoder"

# Shared state
alert_flag = False
connected_flag = False
current_zone = "NONE"
running = True 
calibration_mode_active = False
ble_loop = None 
ble_client_global = None

def draw_ui(screen, font):
    screen.fill((30, 30, 30))

    if connected_flag:
        status = "Connected"
        status_color = (0, 255, 0)
    else:
        status = "Disconnected"
        status_color = (255, 0, 0)
    if calibration_mode_active:
        calibration_text = "CALIBRATION MODE: ON"
        calibration_color = (0, 0, 200)
    else:
        calibration_text = "CALIBRATION MODE: OFF"
        calibration_color = (150, 150, 150)

    if current_zone == "RED":
        alert_text = "Strap is Loose"
        alert_color = (255, 0, 0)
    elif current_zone == "GREEN":
        alert_text = "Strap is Tight"
        alert_color = (0, 200, 0)
    elif current_zone == "YELLOW":
        alert_text = "Strap May Be Loose"
        alert_color = (255, 200, 0)
    else:
        alert_text = "Waiting for Data"
        alert_color = (200, 200, 200)

    status_surface = font.render(status, True, status_color)
    alert_surface = font.render(alert_text, True, alert_color)
    calibration_surface = font.render(calibration_text, True, calibration_color) 

    screen.blit(status_surface, (20, 30))
    screen.blit(alert_surface, (20, 100))
    screen.blit(calibration_surface, (20, 150)) 

    # Draw Calibration button
    pygame.draw.rect(screen, (50, 50, 50), (290, 140, 130, 40))
    button_text_color = (255, 255, 255)
    button_text = "Calibrate" if not calibration_mode_active else "Stop Cal"
    button_surface = font.render(button_text, True, button_text_color)
    screen.blit(button_surface, (300, 145))

    pygame.display.flip()

def notification_handler(sender, data):
    global current_zone
    if not data:
        return
    if data[0] == 0x01:
        current_zone = "RED"
    elif data[0] == 0x02:
        current_zone = "GREEN"
    elif data[0] == 0x03:
        current_zone = "YELLOW"
    elif data[0] == 0x04:
        asyncio.run_coroutine_threadsafe(toggle_calibration_mode(), ble_loop)
    # print(f"Received notification: {data[0]:02x}, current_zone: {current_zone}") # Debugging
    
def on_disconnect(client):
    global connected_flag, current_zone, calibration_mode_active
    print("Device disconnected callback triggered.")
    connected_flag = False
    current_zone = "NONE"
    calibration_mode_active = False 
    

async def toggle_calibration_mode():
    global calibration_mode_active, ble_client_global
    if ble_client_global and ble_client_global.is_connected:
        try:
            new_state = not calibration_mode_active
            value_to_write = b'\x01' if new_state else b'\x00'
            print(f"Attempting to set calibration mode to: {new_state} (value: {value_to_write})")

            # Try both UUID formats for the calibration characteristic
            try:
                await ble_client_global.write_gatt_char(CALIBRATION_CHAR_UUID, value_to_write, response=True)
            except Exception: # Catch broader exceptions during initial write attempt
                await ble_client_global.write_gatt_char(FULL_CALIBRATION_CHAR_UUID, value_to_write, response=True)

            calibration_mode_active = new_state
            print(f"Calibration mode successfully toggled to: {calibration_mode_active}")
        except BleakError as e:
            print(f"Failed to toggle calibration mode: {e}")
        except Exception as e: # Catch any other unexpected errors during write
            print(f"An unexpected error occurred while toggling calibration: {e}")
    else:
        print("Not connected to device, cannot toggle calibration mode.")

async def ble_task():
    global connected_flag, running, current_zone, ble_client_global, calibration_mode_active

    while running:
        print("Scanning for BLE_Encoder...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME)

        if not device:
            print("Device not found. Retrying in 5s...")
            await asyncio.sleep(5)
            continue

        try:
            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                ble_client_global = client # Store client instance
                connected_flag = True
                print("Connected to device")

                # Set initial calibration mode state from device (optional, but good practice)
                calibration_mode_active = False # Reset on new connection, assuming C program defaults to OFF

                try:
                    await client.start_notify(CHAR_UUID, notification_handler)
                except Exception:
                    await client.start_notify(FULL_CHAR_UUID, notification_handler)

                while running and client.is_connected:
                    await asyncio.sleep(0.1)

        except BleakError as e:
            print(f"BLE connection error: {e}")

        # If we reach here, we are disconnected or errored
        connected_flag = False
        current_zone = "NONE"
        calibration_mode_active = False 
        ble_client_global = None 
        print("Disconnected. Reconnecting in 3s...")
        await asyncio.sleep(3)

def start_ble_loop():
    global ble_loop 
    ble_loop = asyncio.new_event_loop() 
    asyncio.set_event_loop(ble_loop) 
    ble_loop.run_until_complete(ble_task()) 

def main():
    global running

    # Start BLE in background thread
    ble_thread = threading.Thread(target=start_ble_loop)
    ble_thread.start()

    # Pygame in main thread
    pygame.init()
    WIDTH, HEIGHT = 500, 250
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("BLE Encoder Monitor")
    font = pygame.font.SysFont(None, 30) 

    clock = pygame.time.Clock()
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.MOUSEBUTTONDOWN:
                # Check if calibration button was clicked
                mouse_pos = event.pos
                # Button area: (250, 140, 130, 40)
                if 290 <= mouse_pos[0] <= (300 + 130) and \
                   140 <= mouse_pos[1] <= (140 + 40):
                    print("Calibration button clicked!")
                    if ble_loop and ble_loop.is_running():
                        asyncio.run_coroutine_threadsafe(toggle_calibration_mode(), ble_loop)
                    else:
                        print("BLE event loop not running, cannot toggle calibration mode.")


        draw_ui(screen, font)
        clock.tick(30)

    pygame.quit()
    ble_thread.join()

if __name__ == "__main__":
    main()