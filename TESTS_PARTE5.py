from pathlib import Path
import re

ROOT = Path(__file__).parent
INO = (ROOT / 'SistemaIntrusos_Parte5.ino').read_text(encoding='utf-8')
APP = (ROOT / 'App.js').read_text(encoding='utf-8')
SERVICE = (ROOT / 'services' / 'esp32.js').read_text(encoding='utf-8')
SCHEMA = (ROOT / 'supabase' / 'migration_part5.sql').read_text(encoding='utf-8')

checks = []
def check(name, condition):
    checks.append((name, bool(condition)))

# 1. Basic source integrity
for a,b,label in [('(',')','parentheses'),('{','}','braces'),('[',']','brackets')]:
    check(f'Firmware balanced {label}', INO.count(a) == INO.count(b))

# 2. Required endpoints and architecture
for endpoint in ['/status','/arm','/disarm','/clear','/capture','/time','/schedule','/stream']:
    check(f'Endpoint {endpoint}', endpoint in INO)
for token in ['xTaskCreatePinnedToCore','cameraCaptureTask','scheduleTask','timeTask','supabaseTask','eventQueueMutex']:
    check(f'Firmware component {token}', token in INO)

# 3. Integration fixes
check('Queue tracks DB state', 'dbRecorded' in INO)
check('Queue tracks email state', 'emailSent' in INO)
check('Duplicate event guard', 'supabaseEventAlreadyRecorded' in INO)
check('Unconfigured SMTP remains pending', 'SMTP sin configurar' in INO)
check('No false email success', 'email_pending' in INO and 'intrusion_processed' in INO)
check('Schedule unique index', 'schedules_device_day_unique' in SCHEMA)

# 4. Expo integration
for token in ['getStatus','armSystem','disarmSystem','syncTime','WebView','supabase.from("intrusion_events")','supabase.from("schedules")']:
    check(f'Expo integration {token}', token in APP)
check('ESP32 service exports stream', 'getStreamUrl' in SERVICE)
check('Expo package has Supabase', '@supabase/supabase-js' in (ROOT/'package.json').read_text())

# 5. No accidentally embedded normal Gmail password label
check('Uses app-password wording', 'CONTRASE' not in INO.upper() or 'CAMBIAR_APP_PASSWORD' in INO)

passed = sum(ok for _,ok in checks)
print(f'Checks: {passed}/{len(checks)} passed')
for name, ok in checks:
    print(('PASS' if ok else 'FAIL') + ' | ' + name)
raise SystemExit(0 if passed == len(checks) else 1)
