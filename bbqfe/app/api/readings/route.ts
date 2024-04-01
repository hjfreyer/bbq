
import admin from 'firebase-admin';
import { FieldValue } from 'firebase-admin/firestore';


if (admin.apps.length === 0) {
  if (process.env.GCP_CREDS) {
    admin.initializeApp({
      credential: admin.credential.cert(JSON.parse(process.env.GCP_CREDS))
    });

  } else if (process.env.BBQ_FIREBASE_SERVICE_ACCOUNT_KEY) {
    admin.initializeApp({
      credential: admin.credential.cert(JSON.parse(process.env.BBQ_FIREBASE_SERVICE_ACCOUNT_KEY))
    });
  }
}

let db = admin.firestore();

export const dynamic = 'force-dynamic'; // static by default, unless reading the request

type Readings = {
  session: number,
  food_temp_f: number,
  ambient_temp_f: number,
  duty_pct: number,
};

export async function POST(request: Request) {
  const readings = await request.json() as Readings;

  const session_ref = db.collection("sessions").doc("" + readings.session);
  const now = new Date();

  session_ref.set(
    {
      last_update: now,
      samples: FieldValue.arrayUnion({
        time: now,
        duty_pct: readings.duty_pct,
        ambient_temp_f: readings.ambient_temp_f,
        food_temp_f: readings.food_temp_f,
      }),
    },
    { merge: true }
  );
  return new Response(`ok\n`);
}
