
import 'https://code.jquery.com/jquery-3.7.1.min.js';

$(() => {
  const bodyHtml = `
    <body>
    <div class="container my-5">
      <h1>BBQ Remote</h1>
      <div class="col-lg-8 px-0">
        <p>Food temp: <span id="food-temp"></span></p>
        <p class="fs-5">You've successfully loaded up the Bootstrap starter example. It includes <a href="https://getbootstrap.com/">Bootstrap 5</a> via the <a href="https://www.jsdelivr.com/package/npm/bootstrap">jsDelivr CDN</a> and includes an additional CSS and JS file for your own code.</p>
        <p>Feel free to download or copy-and-paste any parts of this example.</p>

        <hr class="col-1 my-4">

        <a href="https://getbootstrap.com" class="btn btn-primary">Read the Bootstrap docs</a>
        <a href="https://github.com/twbs/examples" class="btn btn-secondary">View on GitHub</a>
      </div>
    </div>
  </body>`;

  document.body.innerHTML = bodyHtml;

  const foodTemp = $('#food-temp').text('ahoy');

  async function update() {
    const result = await fetch('/settings');

    const json = await result.json();

    foodTemp.text(json['threshold_f'])
  }

  update().then(() => console.log("done"))
})