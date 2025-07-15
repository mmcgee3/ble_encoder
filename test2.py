import asyncio
import threading
import pygame
from bleak import BleakClient, BleakScanner, BleakError

CHAR_UUID = "ff01"
FULL_CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
DEVICE_NAME = "BLE_Encoder"

# Shared state
alert_flag = False
connected_flag = False
current_zone = "NONE"
running = True  # Used to signal exit

def draw_ui(screen, font):
    screen.fill((30, 30, 30))
    
    if connected_flag:
        status = "Connected"
        status_color = (0, 255, 0)
    else:
        status = "Disconnected"
        status_color = (255, 0, 0)
    
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

    screen.blit(status_surface, (20, 30))
    screen.blit(alert_surface, (20, 100))
    pygame.display.flip()

# 0x01 = red, 0x02 = green, 0x03 = yellow
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

async def ble_task():
    global connected_flag, running, current_zone

    while running:
        print("Scanning for BLE_Encoder...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME)

        if not device:
            print("Device not found. Retrying in 5s...")
            await asyncio.sleep(5)
            continue

        try:
            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                connected_flag = True
                print("Connected to device")

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
        print("Disconnected. Reconnecting in 3s...")
        await asyncio.sleep(3)

def on_disconnect(client):
    global connected_flag, current_zone
    print("Device disconnected callback triggered.")
    connected_flag = False
    current_zone = "NONE"

def start_ble_loop():
    asyncio.run(ble_task())

def main():
    global running

    # Start BLE in background thread
    ble_thread = threading.Thread(target=start_ble_loop)
    ble_thread.start()

    # Pygame in main thread
    pygame.init()
    WIDTH, HEIGHT = 400, 200
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("BLE Encoder Monitor")
    font = pygame.font.SysFont(None, 40)

    clock = pygame.time.Clock()
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        draw_ui(screen, font)
        clock.tick(30)

    pygame.quit()
    ble_thread.join()

if __name__ == "__main__":
    main()
