/**
 * Google Sheets bridge for the Smart RFID Attendance System.
 *
 * WHY THIS INSTEAD OF A CONTRIB NODE?
 *   node-red-contrib-google-sheets needs an OAuth service account, a key file and
 *   sheet sharing. This script needs one copy-paste and gives you a plain URL that
 *   any http request node can POST to. Easier for a school project, and it keeps
 *   the Node-RED flow free of Google credentials.
 *
 * SETUP
 *   1. Create a Google Sheet.
 *   2. Extensions -> Apps Script. Delete the sample code, paste this file, save.
 *   3. Deploy -> New deployment -> type "Web app".
 *        Execute as     : Me
 *        Who has access : Anyone        <-- NOT "Anyone with Google account"
 *      Authorise when prompted (click Advanced -> Go to ... (unsafe) -> Allow),
 *      then copy the /exec URL.
 *   4. Paste that URL into the Node-RED flow tab -> Environment -> SHEETS_WEBAPP_URL.
 *
 * CHECK IT
 *   Open the /exec URL in an INCOGNITO window. You should see JSON listing the tabs
 *   in your spreadsheet and which one this script will write to.
 *
 * AFTER ANY EDIT TO THIS FILE
 *   Saving is NOT enough. Deploy -> Manage deployments -> pencil icon ->
 *   Version: New version -> Deploy. Otherwise the web app keeps running the old code.
 *
 * SECURITY NOTE
 *   "Anyone with the link" means anyone who learns the URL can append rows.
 *   Set SHARED_SECRET below to a long random string to stop that, and add the same
 *   value to the Node-RED "Build Sheets row" function as  msg.payload.secret.
 *   Treat the /exec URL itself as a password: never commit it to a public repo.
 */

// The tab this script writes to. If no tab has this name, it falls back to the FIRST
// tab in the spreadsheet, so a renamed or localised tab does not break attendance
// logging. The reply always tells you which tab it actually used.
var SHEET_NAME    = 'Attendance';

var HEADERS       = ['Date', 'Time', 'StudentID', 'Name', 'Class', 'UID', 'Device'];

var SHARED_SECRET = '';   // '' disables the check


/** Returns the sheet to write to, falling back to the first tab. */
function getSheet_() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName(SHEET_NAME);
  if (!sheet) {
    sheet = ss.getSheets()[0];      // graceful fallback
  }
  return sheet;
}

/** Writes the header row if the sheet is completely empty. */
function ensureHeaders_(sheet) {
  if (sheet.getLastRow() === 0) {
    sheet.appendRow(HEADERS);
    return true;
  }
  return false;
}

function doPost(e) {
  try {
    if (!e || !e.postData || !e.postData.contents) {
      return reply({ ok: false, error: 'no POST body received' });
    }

    var body = JSON.parse(e.postData.contents);

    if (SHARED_SECRET && body.secret !== SHARED_SECRET) {
      return reply({ ok: false, error: 'bad secret' });
    }

    var sheet = getSheet_();
    var addedHeaders = ensureHeaders_(sheet);

    sheet.appendRow([
      body.date      || '',
      body.time      || '',
      body.studentId || '',
      body.name      || '',
      body.className || '',
      body.uid       || '',
      body.device    || ''
    ]);

    return reply({
      ok:           true,
      row:          sheet.getLastRow(),
      sheet:        sheet.getName(),
      wroteHeaders: addedHeaders
    });

  } catch (err) {
    return reply({ ok: false, error: String(err) });
  }
}

/**
 * Open the /exec URL in a browser to check the deployment. It lists the tabs in your
 * spreadsheet and the one that will be written to, which makes a wrong tab name
 * obvious straight away.
 */
function doGet() {
  try {
    var ss     = SpreadsheetApp.getActiveSpreadsheet();
    var names  = ss.getSheets().map(function (s) { return s.getName(); });
    var target = getSheet_();

    return reply({
      ok:            true,
      service:       'rfid-attendance-sheets-bridge',
      spreadsheet:   ss.getName(),
      tabsFound:     names,
      willWriteTo:   target.getName(),
      exactNameUsed: names.indexOf(SHEET_NAME) !== -1
    });
  } catch (err) {
    return reply({ ok: false, error: String(err) });
  }
}

function reply(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
