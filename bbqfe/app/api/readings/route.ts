export const dynamic = 'force-dynamic'; // static by default, unless reading the request
 
export function GET(request: Request) {
  return new Response(`Hello from ${process.env.FOO}`);
}

export async function POST(request: Request) {
  console.log(await request.json());
  return new Response(`POSTED`);
}
