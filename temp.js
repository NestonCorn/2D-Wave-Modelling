
        // UI Tab Switcher Function
        function switchTab(tabId, el) {
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.getElementById('tab-' + tabId).classList.add('active');
            el.classList.add('active');
        }

        // --- Parameter Save & Load System ---
        function getParameterConfig() {
            const ids = [
                'sl_fdOrder', 'sl_dh', 'sl_dt', 'sl_npml', 'sl_freq', 'sl_amp',
                'sl_cable', 'sl_groupSpace', 'sl_cableHead', 'sl_cableTail',
                'sl_srcDepth', 'shipXSlider', 'sl_srcOff', 'sl_shipSrc', 'sl_srcRec',
                'sl_shipSpeed', 'sl_shot', 'sl_geoModel', 'sl_uiWidth', 'sl_uiHeight',
                'sl_obsSpace', 'sl_gain', 'sl_speed', 'sismoTimeSlider',
                'segyLenVal'
            ];
            let config = {};
            ids.forEach(id => {
                let el = document.getElementById(id);
                if (el) config[id] = el.value;
            });
            config['obs_enabled'] = ui_obs_enabled;
            return config;
        }

        function applyParameterConfig(config) {
            if (!config) return;
            for (let id in config) {
                if (id === 'obs_enabled') continue;
                let el = document.getElementById(id);
                if (el) {
                    el.value = config[id];
                    el.dispatchEvent(new Event('input', { bubbles: true }));
                    el.dispatchEvent(new Event('change', { bubbles: true }));
                }
            }
            if (config['obs_enabled'] !== undefined && config['obs_enabled'] !== ui_obs_enabled) {
                toggleOBS();
            }
        }

        function saveParameters(type) {
            let config = getParameterConfig();
            let jsonStr = JSON.stringify(config, null, 2);
            if (type === 'local') {
                localStorage.setItem('seismic_params', jsonStr);
                alert("Parametreler tarayıcıya (Local Storage) başarıyla kaydedildi.");
            } else if (type === 'file') {
                let blob = new Blob([jsonStr], { type: 'application/json' });
                let url = URL.createObjectURL(blob);
                let a = document.createElement('a');
                a.href = url;
                a.download = 'seismic_parameters.json';
                a.click();
                URL.revokeObjectURL(url);
            }
        }

        function loadParameters(type) {
            if (type === 'local') {
                let jsonStr = localStorage.getItem('seismic_params');
                if (jsonStr) {
                    try {
                        let config = JSON.parse(jsonStr);
                        applyParameterConfig(config);
                        alert("Parametreler yerel hafızadan yüklendi!");
                    } catch (e) {
                        alert("Kayıt okunamadı veya bozuk!");
                    }
                } else {
                    alert("Yerel hafızada kayıtlı parametre bulunamadı.");
                }
            }
        }

        function loadParametersFromFile(event) {
            let file = event.target.files[0];
            if (!file) return;
            let reader = new FileReader();
            reader.onload = function(e) {
                try {
                    let config = JSON.parse(e.target.result);
                    applyParameterConfig(config);
                    alert("Parametreler dosyadan başarıyla yüklendi!");
                } catch (err) {
                    alert("Geçersiz JSON dosyası!");
                }
            };
            reader.readAsText(file);
            event.target.value = ''; // Reset input to allow reloading same file
        }
        // ------------------------------------
    
