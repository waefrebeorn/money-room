// Money Room — shared site JavaScript
// Data loading with error boundaries and retry
function fetchData(url, retries) {
  retries = retries || 2;
  return fetch(url).then(function(r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).catch(function(e) {
    if (retries > 0) return fetchData(url, retries - 1);
    throw e;
  });
}

function setEl(id, val) {
  var el = document.getElementById(id);
  if (el) el.textContent = val;
}

// Loading state helper
function showLoading(id) {
  var el = document.getElementById(id);
  if (el) el.innerHTML = '<span style="color:var(--text-quaternary);animation:pulse 1.5s infinite">Loading...</span>';
}

function showError(id, msg) {
  var el = document.getElementById(id);
  if (el) el.innerHTML = '<span style="color:var(--red)">' + (msg || 'Error loading data') + '</span>';
}

// Mobile nav toggle
document.addEventListener('DOMContentLoaded', function() {
  var nav = document.querySelector('.nav-links');
  if (nav && window.innerWidth < 768) {
    var toggle = document.createElement('button');
    toggle.innerHTML = '☰';
    toggle.style.cssText = 'background:none;border:none;color:var(--text);font-size:24px;cursor:pointer;display:none;padding:8px';
    toggle.onclick = function() { nav.style.display = nav.style.display === 'flex' ? 'none' : 'flex'; };
    document.querySelector('nav .container').appendChild(toggle);
  }
});
