import http.server
import socketserver
import sys

PORT = 8000

import os

class SecureHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        # Betiğin çalıştığı klasörün bir üstünü HTML dosyalarının kök konumu olarak ayarla
        base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        super().__init__(*args, directory=base_dir, **kwargs)

    def end_headers(self):
        # Güvenlik başlıkları: SharedArrayBuffer için zorunludur.
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        super().end_headers()

if __name__ == "__main__":
    with socketserver.TCPServer(("", PORT), SecureHandler) as httpd:
        print(f"===========================================================")
        print(f"🚀 MULTI-CORE FDTD SUNUCUSU BAŞLATILDI")
        print(f"===========================================================")
        print(f"Tarayıcınızdan şu adrese gidin: http://localhost:{PORT}")
        print(f"Bu sunucu acikkaldigi surece CPU cekirdekleri FDTD hesabinda kullanilacaktir.")
        print(f"Kapatmak icin CTRL+C basin.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nSunucu kapatiliyor...")
            sys.exit(0)
