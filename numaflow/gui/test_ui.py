#!/usr/bin/env python3
"""Optional end-to-end/visual-smoke test: make -C numaflow && python3 numaflow/gui/test_ui.py

Requires Selenium, Firefox and geckodriver. Saves desktop/mobile screenshots to
NF_SCREENSHOT_DIR (or a temporary directory). Does not require Redis.
"""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from selenium import webdriver
from selenium.webdriver.common.action_chains import ActionChains
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import Select, WebDriverWait

ROOT = Path(__file__).resolve().parent
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    PORT = sock.getsockname()[1]
BASE = f"http://127.0.0.1:{PORT}"
# Snap Firefox cannot read its temporary profiles under /tmp on some systems.
if Path('/snap/bin/firefox').exists():
    snap_tmp = Path.home() / 'snap/firefox/common'
    snap_tmp.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(snap_tmp)
shots = Path(os.environ.get('NF_SCREENSHOT_DIR', os.environ['TMPDIR'] if 'TMPDIR' in os.environ else tempfile.gettempdir()))
shots.mkdir(parents=True, exist_ok=True)

server = subprocess.Popen(['python3', str(ROOT / 'server.py')], env={**os.environ, 'PORT': str(PORT)}, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
driver = None
try:
    for _ in range(80):
        try:
            with urllib.request.urlopen(BASE + '/api/ops') as response:
                catalog = json.load(response)
            break
        except (urllib.error.URLError, ConnectionError):
            time.sleep(.1)
    else:
        raise RuntimeError('GUI server did not start; build with make -C numaflow first')
    assert len(catalog) == 7, 'the public palette must contain exactly seven actions'
    with urllib.request.urlopen(BASE + '/api/ops/legacy') as response:
        assert len(json.load(response)) >= 40, 'legacy templates must stay readable'
    invalid = urllib.request.Request(BASE+'/api/run', b'{', {'Content-Type': 'application/json'}, method='POST')
    try:
        urllib.request.urlopen(invalid)
        raise AssertionError('invalid JSON should be rejected')
    except urllib.error.HTTPError as e:
        assert e.code == 400
    options = Options()
    options.add_argument('-headless')
    driver = webdriver.Firefox(service=Service(shutil.which('geckodriver')), options=options)
    driver.set_window_size(1440, 900)
    driver.get(BASE)
    wait = WebDriverWait(driver, 15)
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.node')) == 3)
    assert len(driver.find_elements(By.CSS_SELECTOR, '.action-card')) == 7
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 2
    assert driver.find_element(By.CSS_SELECTOR, '.node').rect['width'] >= 160, 'starter nodes must be legible'
    canvas = driver.find_element(By.ID, 'canvas').rect
    assert canvas['width'] >= 500 and canvas['height'] >= 300, 'editor canvas must have usable space'
    assert driver.save_screenshot(str(shots/'numaflow-desktop.png'))
    assert (shots/'numaflow-desktop.png').stat().st_size > 30_000, 'desktop screenshot must contain rendered UI'
    search = driver.find_element(By.ID, 'actionSearch')
    search.send_keys('CXL')
    assert len(driver.find_elements(By.CSS_SELECTOR, '.action-card')) == 1
    search.send_keys(Keys.CONTROL, 'a', Keys.BACKSPACE)
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.action-card')) == 7)

    # A cycle is blocked; a new cross-connection is accepted.
    def drag_port(src, dst):
        a = driver.find_element(By.CSS_SELECTOR, f'.node[data-id="{src}"] .port.output')
        b = driver.find_element(By.CSS_SELECTOR, f'.node[data-id="{dst}"] .port.input')
        ActionChains(driver).move_to_element(a).click_and_hold().move_to_element(b).release().perform()
    drag_port('n3', 'n1')
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 2, 'cycles must be refused'
    drag_port('n1', 'n3')
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 3
    ActionChains(driver).move_to_element(driver.find_element(By.CSS_SELECTOR, '.edge-hit')).click().perform()
    wait.until(lambda d: 'Remove connection' in d.find_element(By.ID, 'inspectorBody').text)
    driver.find_element(By.CSS_SELECTOR, '.danger').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 2

    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: 'execution=OK' in d.find_element(By.ID, 'outputText').text)
    assert 'nodes=3' in driver.find_element(By.ID, 'outputText').text
    driver.find_element(By.CSS_SELECTOR, '.node[data-id="n1"] .node-card').click()
    wait.until(lambda d: d.find_element(By.ID, 'modeSelect'))
    Select(driver.find_element(By.ID, 'modeSelect')).select_by_value('benefit')
    assert driver.find_element(By.CSS_SELECTOR, '.node[data-id="n1"] .node-subtitle').text == 'Migration benefit'
    driver.find_element(By.CSS_SELECTOR, '.action-card[aria-label="Add Track activity"]').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.node')) == 4
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 3, 'adding after selection should connect it'
    driver.find_element(By.CSS_SELECTOR, '.danger').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.node')) == 3

    wait.until(lambda d: len(Select(d.find_element(By.ID, 'templateSelect')).options) > 2)
    Select(driver.find_element(By.ID, 'templateSelect')).select_by_index(1)
    driver.find_element(By.ID, 'loadTemplateBtn').click()
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.node')) > 3)
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').text, 'legacy nodes must have labels'
    assert driver.find_element(By.CSS_SELECTOR, '.node').rect['width'] >= 170, 'long templates should open at readable scale'
    driver.find_element(By.ID, 'fitBtn').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.node')) > 3, 'fit must preserve template nodes'

    driver.find_element(By.ID, 'newBtn').click()
    driver.switch_to.alert.accept()
    wait.until(lambda d: not d.find_elements(By.CSS_SELECTOR, '.node'))
    assert driver.find_element(By.ID, 'emptyState').is_displayed()
    driver.find_element(By.ID, 'emptyAdd').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.node')) == 1
    driver.set_window_size(390, 844)
    time.sleep(.3)
    assert driver.execute_script('return document.documentElement.scrollWidth <= window.innerWidth + 2'), 'mobile page must not overflow horizontally'
    assert driver.find_element(By.ID, 'canvas').rect['height'] >= 170
    assert driver.find_element(By.ID, 'canvas').rect['width'] >= 300, 'mobile canvas must not collapse'
    assert driver.save_screenshot(str(shots/'numaflow-mobile.png'))
    assert (shots/'numaflow-mobile.png').stat().st_size > 20_000, 'mobile screenshot must contain rendered UI'
    # Real file import: legacy IDs remain executable, bad graphs leave the
    # current flow unchanged rather than overwriting it with partial data.
    import_file = shots/'numaflow-import-test.json'
    import_file.write_text(json.dumps({'name': 'Imported legacy', 'nodes': [
        {'id': 'legacy1', 'op': 'filter_hot', 'params': {'threshold': '1'}}
    ], 'edges': []}), encoding='utf-8')
    driver.find_element(By.ID, 'fileInput').send_keys(str(import_file))
    wait.until(lambda d: d.find_element(By.ID, 'workflowName').get_attribute('value') == 'Imported legacy')
    wait.until(lambda d: d.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent') == 'Filter: Hot')
    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: 'execution=OK' in d.find_element(By.ID, 'outputText').text)
    import_file.write_text(json.dumps({'name': 'Invalid cycle', 'nodes': [
        {'id': 'a', 'op': 'place_items'}, {'id': 'b', 'op': 'move_items'}
    ], 'edges': [{'from': 'a', 'to': 'b'}, {'from': 'b', 'to': 'a'}]}), encoding='utf-8')
    driver.find_element(By.ID, 'fileInput').send_keys(str(import_file))
    assert driver.find_element(By.ID, 'workflowName').get_attribute('value') == 'Imported legacy'
    import_file.unlink()
    print('GUI browser/visual checks passed; screenshots: ' + str(shots))
finally:
    if driver:
        driver.quit()
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
