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
 *   1. Create a Google Sheet. Name the first tab "Attendance".
 *   2. Row 1 headers:  Date | Time | StudentID | Name | Class | UID | Device
 *   3. Extensions -> Apps Script. Delete the sample code, paste this file, save.
 *   4. Deploy -> New deployment -> type "Web app".
 *        Execute as        : Me
 *        Who has access    : Anyone with the link
 *      Authorise when prompted, then copy the /exec URL.
 *   5. Before starting Node-RED, set the environment variable:
 *        Linux/macOS :  export SHEETS_WEBAPP_URL="https://script.google.com/.../exec"
 *        Windows     :  setx SHEETS_WEBAPP_URL "https://script.google.com/.../exec"
 *
 * SECURITY NOTE
 *   "Anyone with the link" means anyone who learns the URL can append rows.
 *   The SHARED_SECRET below closes that gap - set the same value in Node-RED's
 *   "Build Sheets row" function (add secret: 'your-secret' to msg.payload).
 *   Treat the /exec URL itself as a password: never commit it to a public repo.
 */

var SHEET_NAME    = 'Attendance';
var SHARED_SECRET = '';   // leave '' to disable the check, or set a long random string

function doPost(e) {
  try {
    var body = JSON.parse(e.postData.contents);

    if (SHARED_SECRET && body.secret !== SHARED_SECRET) {
      return reply({ ok: false, error: 'bad secret' });
    }

    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
    if (!sheet) {
      return reply({ ok: false, error: 'sheet "' + SHEET_NAME + '" not found' });
    }

    sheet.appendRow([
      body.date      || '',
      body.time      || '',
      body.studentId || '',
      body.name      || '',
      body.className || '',
      body.uid       || '',
      body.device    || ''
    ]);

    return reply({ ok: true, row: sheet.getLastRow() });

  } catch (err) {
    return reply({ ok: false, error: String(err) });
  }
}

/** Lets you confirm the deployment is live by opening the /exec URL in a browser. */
function doGet() {
  return reply({ ok: true, service: 'rfid-attendance-sheets-bridge' });
}

function reply(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
