void printNfcNdefRecords(NdefMessage message) {
  int recordCount = message.getRecordCount();
  for (int i = 0; i < recordCount; i++) {
    NdefRecord record = message.getRecord(i);
    // NdefRecord record = message[i]; // alternate syntax

    Serial.print("  TNF: ");
    Serial.println(record.getTnf());
    Serial.print("  Type: ");
    Serial.println(getHexString(record.getType(), record.getTypeLength()));

    // The TNF and Type should be used to determine how your
    // application processes the payload There's no generic processing
    // for the payload, it's returned as a byte[]
    int payloadLength = record.getPayloadLength();
    const byte* payload = record.getPayload();

    // Print the Hex and Printable Characters
    Serial.print("  Payload (HEX): ");
    Serial.println(getHexString(payload, payloadLength));

    // Force the data into a String (might work depending on the
    // content) Real code should use smarter processing
    String payloadAsString = "";
    for (int c = 0; c < payloadLength; c++) {
      payloadAsString += (char)payload[c];
    }
    Serial.print("  Payload (as String): ");
    Serial.println(payloadAsString);

    // id is probably blank and will return ""
    if (record.getIdLength() > 0) {
      Serial.print("  ID: ");
      Serial.println(getHexString(record.getId(), record.getIdLength()));
    }
  }
}

String getHexString(const byte* buffer, byte bufferSize) {
  String id = "";
  for (byte i = 0; i < bufferSize; i++) {
    id += buffer[i] < 0x10 ? "0" : "";
    id += String(buffer[i], HEX);
  }
  return id;
}

bool compareUid(MFRC522::Uid& uid1, MFRC522::Uid& uid2) {
  if (uid1.size != uid2.size) return false;
  for (byte i = 0; i < uid1.size; i++) {
    if (uid1.uidByte[i] != uid2.uidByte[i]) return false;
  }
  return true;
}