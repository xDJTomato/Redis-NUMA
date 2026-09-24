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
    assert len(driver.find_elements(By.CSS_SELECTOR, '#actionList .action-card')) == 7
    assert len(driver.find_elements(By.CSS_SELECTOR, '#legacyActionList .legacy-card')) == 36
    assert not driver.find_element(By.ID, 'legacyLibrary').get_attribute('open')
    assert len(driver.find_elements(By.CSS_SELECTOR, '.edge')) == 2
    assert driver.find_element(By.CSS_SELECTOR, '.node').rect['width'] >= 160, 'starter nodes must be legible'
    canvas = driver.find_element(By.ID, 'canvas').rect
    assert canvas['width'] >= 500 and canvas['height'] >= 300, 'editor canvas must have usable space'
    assert driver.save_screenshot(str(shots/'numaflow-desktop.png'))
    assert (shots/'numaflow-desktop.png').stat().st_size > 30_000, 'desktop screenshot must contain rendered UI'
    # Localization covers static UI, modes, detailed node guidance and templates
    # without changing the graph's language-neutral op/mode IDs.
    before = driver.execute_script('return toWorkflow()')
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('zh')
    assert driver.find_element(By.TAG_NAME, 'html').get_attribute('lang') == 'zh-CN'
    assert driver.find_element(By.CSS_SELECTOR, '.crumb').text == '工作流工作台'
    assert driver.find_element(By.CSS_SELECTOR, '#actionList .action-card').get_attribute('aria-label').startswith('添加')
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent') == '计算评分'
    after = driver.execute_script('return toWorkflow()')
    assert before['nodes'] == after['nodes'] and before['edges'] == after['edges'], 'language must not change the DAG'
    driver.find_element(By.CSS_SELECTOR, '.node[data-id="n1"] .node-card').click()
    assert '节点作用' in driver.find_element(By.CSS_SELECTOR, '.node-guide').text
    assert '阶梯衰减' in driver.find_element(By.CSS_SELECTOR, '.guide-mode').text
    assert Select(driver.find_element(By.ID, 'modeSelect')).first_selected_option.text == '热度'
    assert driver.save_screenshot(str(shots/'numaflow-zh-desktop.png'))
    assert (shots/'numaflow-zh-desktop.png').stat().st_size > 30_000
    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: '执行成功：输出对象' in d.find_element(By.ID, 'outputText').text)
    assert 'execution=OK' in driver.find_element(By.ID, 'outputText').text  # original engine output retained
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('en')
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent') == 'Score items'
    search = driver.find_element(By.ID, 'actionSearch')
    search.send_keys('CXL')
    assert len(driver.find_elements(By.CSS_SELECTOR, '#actionList .action-card')) == 1
    search.send_keys(Keys.CONTROL, 'a', Keys.BACKSPACE)
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '#actionList .action-card')) == 7)

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
    driver.find_element(By.CSS_SELECTOR, '#actionList .action-card[aria-label="Add Track activity"]').click()
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
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('zh')
    assert driver.find_elements(By.CSS_SELECTOR, '#templateSelect optgroup[label="内存分层"]')
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent').startswith('旧版')
    driver.find_element(By.CSS_SELECTOR, '.node .node-card').click()
    assert driver.find_element(By.CSS_SELECTOR, '.node-guide').text
    # Compatibility operations keep their original mark-only semantics, and
    # their Chinese descriptions must not suggest they migrate immediately.
    Select(driver.find_element(By.ID, 'templateSelect')).select_by_value('tier_demote_cold')
    driver.find_element(By.ID, 'loadTemplateBtn').click()
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.node')) == 3)
    driver.find_element(By.CSS_SELECTOR, '.node[data-id="n2"] .node-card').click()
    assert '只标记' in driver.find_element(By.CSS_SELECTOR, '.node-guide').text
    assert '当前节点不会改变' in driver.find_element(By.CSS_SELECTOR, '.node-guide').text
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('en')
    driver.find_element(By.ID, 'fitBtn').click()
    assert len(driver.find_elements(By.CSS_SELECTOR, '.node')) == 3, 'fit must preserve template nodes'

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
    assert driver.find_element(By.ID, 'actionSearch').is_displayed(), 'legacy presets must be searchable on mobile'
    assert driver.save_screenshot(str(shots/'numaflow-mobile.png'))
    assert (shots/'numaflow-mobile.png').stat().st_size > 20_000, 'mobile screenshot must contain rendered UI'
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('zh')
    assert driver.find_element(By.ID, 'canvas').rect['width'] >= 300
    assert driver.save_screenshot(str(shots/'numaflow-zh-mobile.png'))
    assert (shots/'numaflow-zh-mobile.png').stat().st_size > 20_000
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('en')
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
    # Saved language survives reload; imported workflows are not retranslated.
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('zh')
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent').startswith('旧版')
    driver.refresh()
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.node')) == 3)
    assert Select(driver.find_element(By.ID, 'languageSelect')).first_selected_option.get_attribute('value') == 'zh'
    assert driver.find_element(By.CSS_SELECTOR, '.node-title').get_attribute('textContent') == '计算评分'
    driver.set_window_size(1440, 900)
    Select(driver.find_element(By.ID, 'languageSelect')).select_by_value('en')
    driver.find_element(By.ID, 'newBtn').click()
    driver.switch_to.alert.accept()
    search = driver.find_element(By.ID, 'actionSearch')
    search.send_keys('demote_cold')
    assert driver.find_element(By.ID, 'legacyLibrary').get_attribute('open')
    assert len(driver.find_elements(By.CSS_SELECTOR, '#legacyActionList .legacy-card')) == 1
    driver.find_element(By.CSS_SELECTOR, '.legacy-card[data-preset="demote_cold"]').click()
    wf = driver.execute_script('return toWorkflow()')
    assert wf['nodes'][0]['op'] == 'move_items' and wf['nodes'][0]['params']['mode'] == 'demote_mark'
    assert wf['nodes'][0]['preset'] == 'demote_cold'
    assert not driver.find_element(By.ID, 'modeSelect').is_enabled()
    assert not driver.find_element(By.ID, 'field-threshold').is_enabled()
    assert 'does not migrate items' in driver.find_element(By.CSS_SELECTOR, '.node-guide').text
    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: 'execution=OK' in d.find_element(By.ID, 'outputText').text)
    # An exported preset stays fixed after file import; the engine ignores the
    # optional UI-only marker but still executes its canonical op/mode pair.
    import_file = shots/'numaflow-fixed-preset.json'
    import_file.write_text(json.dumps(wf), encoding='utf-8')
    driver.find_element(By.ID, 'fileInput').send_keys(str(import_file))
    wait.until(lambda d: d.find_element(By.CSS_SELECTOR, '.node.preset'))
    driver.find_element(By.CSS_SELECTOR, '.node .node-card').click()
    assert not driver.find_element(By.ID, 'modeSelect').is_enabled()
    driver.find_element(By.CSS_SELECTOR, '.preset-banner .secondary-action').click()
    assert 'preset' not in driver.execute_script('return toWorkflow()')['nodes'][0]
    assert driver.find_element(By.ID, 'modeSelect').is_enabled()
    import_file.unlink()
    # Runtime-dependent defaults must not be frozen to the test machine's node
    # or budget merely because the UI displays a fixed preset.
    driver.find_element(By.ID, 'newBtn').click()
    driver.switch_to.alert.accept()
    search = driver.find_element(By.ID, 'actionSearch')
    search.send_keys(Keys.CONTROL, 'a', Keys.BACKSPACE)
    search.send_keys('filter_remote')
    driver.find_element(By.CSS_SELECTOR, '.legacy-card[data-preset="filter_remote"]').click()
    assert 'runtime' in driver.find_element(By.ID, 'field-node').get_attribute('value').lower()
    assert 'node' not in driver.execute_script('return toWorkflow()')['nodes'][0]['params']
    search.send_keys(Keys.CONTROL, 'a', Keys.BACKSPACE)
    search.send_keys('budget_limit')
    driver.find_element(By.CSS_SELECTOR, '.legacy-card[data-preset="budget_limit"]').click()
    assert 'runtime' in driver.find_element(By.ID, 'field-budget').get_attribute('value').lower()
    assert 'budget' not in driver.execute_script('return toWorkflow()')['nodes'][1]['params']
    # Convert an imported old operation in place, keeping edges and threshold.
    Select(driver.find_element(By.ID, 'templateSelect')).select_by_value('tier_demote_cold')
    driver.find_element(By.ID, 'loadTemplateBtn').click()
    wait.until(lambda d: len(d.find_elements(By.CSS_SELECTOR, '.node')) == 3)
    driver.find_element(By.CSS_SELECTOR, '.node[data-id="n2"] .node-card').click()
    before = driver.execute_script('return toWorkflow()')
    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: 'execution=OK' in d.find_element(By.ID, 'outputText').text)
    original_output = driver.find_element(By.ID, 'outputText').text
    driver.find_element(By.CSS_SELECTOR, '#inspectorBody .secondary-action').click()
    after = driver.execute_script('return toWorkflow()')
    assert after['edges'] == before['edges']
    assert after['nodes'][1]['op'] == 'move_items'
    assert after['nodes'][1]['params'] == {**before['nodes'][1]['params'], 'mode':'demote_mark'}
    assert after['nodes'][1]['preset'] == 'demote_cold'
    driver.find_element(By.ID, 'runBtn').click()
    wait.until(lambda d: 'execution=OK' in d.find_element(By.ID, 'outputText').text)
    assert driver.find_element(By.ID, 'outputText').text == original_output, 'conversion must preserve execution results'
    # Malformed preset metadata must be rejected instead of displaying a
    # misleading locked badge or changing a previously imported workflow.
    bad = shots/'numaflow-invalid-preset.json'
    corrupted = json.loads(json.dumps(after))
    corrupted['nodes'][1]['preset'] = 'balance_nodes'
    bad.write_text(json.dumps(corrupted), encoding='utf-8')
    driver.find_element(By.ID, 'fileInput').send_keys(str(bad))
    assert driver.execute_script('return toWorkflow()') == after
    bad.unlink()
    print('GUI browser/visual checks passed (EN + 简体中文 + legacy presets); screenshots: ' + str(shots))
finally:
    if driver:
        driver.quit()
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
